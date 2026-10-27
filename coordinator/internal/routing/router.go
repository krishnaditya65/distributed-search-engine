// Package routing implements api.Coordinator: it is the glue between the
// REST layer (internal/api), the replicated cluster state (internal/cluster,
// backed by Raft), and the C++ engine nodes (internal/engineclient,
// internal/replication). This is where document-to-shard routing, the
// scatter-gather search fan-out, and distributed-aggregation merging happen.
package routing

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"hash/fnv"
	"time"

	"github.com/google/uuid"

	"dse/coordinator/internal/api"
	"dse/coordinator/internal/cluster"
	"dse/coordinator/internal/engineclient"
	"dse/coordinator/internal/genproto"
	"dse/coordinator/internal/replication"
)

const applyTimeout = 5 * time.Second

// Router implements api.Coordinator.
type Router struct {
	node *cluster.Node
	pool *engineclient.Pool
}

// NewRouter builds a Router over the given raft node and engine connection
// pool.
func NewRouter(node *cluster.Node, pool *engineclient.Pool) *Router {
	return &Router{node: node, pool: pool}
}

// shardForKey deterministically maps a document key to one of numShards
// shard indices via FNV-1a hashing, so repeated calls (index, then get,
// then delete) for the same key always land on the same shard.
func shardForKey(key string, numShards int) int {
	h := fnv.New32a()
	_, _ = h.Write([]byte(key))
	return int(h.Sum32() % uint32(numShards))
}

func (r *Router) indexMeta(index string) (*cluster.IndexMeta, error) {
	state := r.node.FSM.State()
	meta, ok := state.Indices[index]
	if !ok {
		return nil, api.ErrIndexNotFound
	}
	return meta, nil
}

// ---------------------------------------------------------------------------
// CreateIndex
// ---------------------------------------------------------------------------

func (r *Router) CreateIndex(ctx context.Context, index string, req api.CreateIndexRequest) error {
	if !r.node.IsLeader() {
		return fmt.Errorf("this node is not the raft leader (leader is %q); retry against the leader",
			r.node.LeaderAddr())
	}

	state := r.node.FSM.State()
	if len(state.EngineNodes) == 0 {
		return errors.New("no engine nodes registered with the cluster")
	}

	numShards := req.Shards
	if numShards <= 0 {
		numShards = 6
	}
	numReplicas := req.Replicas
	if numReplicas < 0 {
		numReplicas = 0
	}

	mappings := make([]cluster.FieldMapping, 0, len(req.Mappings))
	for _, m := range req.Mappings {
		mappings = append(mappings, cluster.FieldMapping{Field: m.Field, Type: m.Type, VectorDim: m.VectorDim})
	}

	cmd := cluster.Command{
		Type: cluster.CmdCreateIndex,
		CreateIndex: &cluster.CreateIndexCommand{
			Name:        index,
			Mappings:    mappings,
			NumShards:   numShards,
			NumReplicas: numReplicas,
			EngineNodes: state.EngineNodes,
		},
	}
	if _, err := r.node.Apply(cmd, applyTimeout); err != nil {
		return fmt.Errorf("commit create-index to raft log: %w", err)
	}

	// Now that the cluster metadata is durably committed, physically create
	// the shard on every engine node that owns a copy of it (primary +
	// replicas). This is a best-effort step layered ON TOP of the
	// raft-committed metadata: if it partially fails, the index metadata
	// still exists (a real system would need a saga/reconciliation loop to
	// retry shard creation on lagging nodes — not implemented here, see
	// DOCUMENTATION.md "Known simplifications").
	meta, err := r.indexMeta(index)
	if err != nil {
		return err
	}
	pbMappings := make([]*genproto.FieldMapping, 0, len(mappings))
	for _, m := range mappings {
		pbMappings = append(pbMappings, &genproto.FieldMapping{
			Field: m.Field, Type: m.Type, VectorDim: int32(m.VectorDim),
		})
	}
	for _, shard := range meta.Shards {
		createReq := &genproto.CreateShardRequest{ShardId: shard.ShardID, Mappings: pbMappings}
		if err := replication.CreateShard(ctx, r.pool, shard, createReq); err != nil {
			return fmt.Errorf("index %q metadata committed but shard creation failed: %w", index, err)
		}
	}
	return nil
}

// ---------------------------------------------------------------------------
// Document CRUD
// ---------------------------------------------------------------------------

func (r *Router) IndexDocument(ctx context.Context, index, docID string, body json.RawMessage) (int64, error) {
	meta, err := r.indexMeta(index)
	if err != nil {
		return 0, err
	}
	if docID == "" {
		docID = uuid.NewString()
	}
	shard := meta.Shards[shardForKey(docID, len(meta.Shards))]

	resp, err := replication.IndexDocument(ctx, r.pool, shard, &genproto.IndexRequest{
		ShardId:  shard.ShardID,
		Document: &genproto.Document{Id: docID, Json: string(body)},
	})
	if err != nil {
		return 0, err
	}
	if !resp.GetSuccess() {
		return 0, errors.New(resp.GetError())
	}
	return resp.GetVersion(), nil
}

func (r *Router) GetDocument(ctx context.Context, index, docID string) (json.RawMessage, bool, error) {
	meta, err := r.indexMeta(index)
	if err != nil {
		return nil, false, err
	}
	shard := meta.Shards[shardForKey(docID, len(meta.Shards))]
	client, err := r.pool.Get(shard.Primary)
	if err != nil {
		return nil, false, err
	}
	resp, err := client.API().GetDocument(ctx, &genproto.GetRequest{ShardId: shard.ShardID, DocId: docID})
	if err != nil {
		return nil, false, err
	}
	if !resp.GetFound() {
		return nil, false, nil
	}
	return []byte(resp.GetDocument().GetJson()), true, nil
}

func (r *Router) DeleteDocument(ctx context.Context, index, docID string) (bool, error) {
	meta, err := r.indexMeta(index)
	if err != nil {
		return false, err
	}
	shard := meta.Shards[shardForKey(docID, len(meta.Shards))]
	resp, err := replication.DeleteDocument(ctx, r.pool, shard, &genproto.DeleteRequest{
		ShardId: shard.ShardID, DocId: docID,
	})
	if err != nil {
		return false, err
	}
	return resp.GetFound(), nil
}

func (r *Router) BulkIndex(ctx context.Context, index string, ops []api.BulkOp) ([]api.BulkOpResult, error) {
	meta, err := r.indexMeta(index)
	if err != nil {
		return nil, err
	}
	numShards := len(meta.Shards)

	// Assign each op a doc id up front (so index-op auto-ids are stable
	// across the shard-grouping and result-reassembly steps below) and
	// bucket ops by target shard index, remembering each op's original
	// position so the response can be reassembled in request order.
	type placedOp struct {
		origIndex int
		docID     string
		op        api.BulkOp
	}
	buckets := make(map[int][]placedOp)
	for i, op := range ops {
		docID := op.DocID
		if docID == "" && op.Type == "index" {
			docID = uuid.NewString()
		}
		shardIdx := shardForKey(docID, numShards)
		buckets[shardIdx] = append(buckets[shardIdx], placedOp{origIndex: i, docID: docID, op: op})
	}

	results := make([]api.BulkOpResult, len(ops))
	for shardIdx, placed := range buckets {
		shard := meta.Shards[shardIdx]
		pbOps := make([]*genproto.BulkOp, 0, len(placed))
		for _, po := range placed {
			opType := genproto.BulkOp_INDEX
			if po.op.Type == "delete" {
				opType = genproto.BulkOp_DELETE
			}
			pbOps = append(pbOps, &genproto.BulkOp{Type: opType, DocId: po.docID, Json: string(po.op.Body)})
		}
		resp, err := replication.BulkIndex(ctx, r.pool, shard, &genproto.BulkRequest{
			ShardId: shard.ShardID, Ops: pbOps,
		})
		if err != nil {
			for _, po := range placed {
				results[po.origIndex] = api.BulkOpResult{Success: false, Error: err.Error(), DocID: po.docID}
			}
			continue
		}
		for i, po := range placed {
			var rr *genproto.BulkResult
			if i < len(resp.GetResults()) {
				rr = resp.GetResults()[i]
			}
			if rr == nil {
				results[po.origIndex] = api.BulkOpResult{Success: false, Error: "no result from engine", DocID: po.docID}
				continue
			}
			results[po.origIndex] = api.BulkOpResult{Success: rr.GetSuccess(), Error: rr.GetError(), DocID: rr.GetDocId()}
		}
	}
	return results, nil
}

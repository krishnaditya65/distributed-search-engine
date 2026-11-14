package routing

import (
	"context"
	"fmt"
	"sort"
	"sync"
	"time"

	"dse/coordinator/internal/api"
	"dse/coordinator/internal/cluster"
	"dse/coordinator/internal/genproto"
)

const inspectTimeout = 3 * time.Second

// ListIndices summarises every index with live document counts from the shard primaries.
func (r *Router) ListIndices(ctx context.Context) ([]api.IndexSummary, error) {
	state := r.node.FSM.State()
	names := make([]string, 0, len(state.Indices))

	for name := range state.Indices {
		names = append(names, name)
	}

	sort.Strings(names)

	summaries := make([]api.IndexSummary, len(names))

	var group sync.WaitGroup

	for position, name := range names {
		group.Add(1)

		go func() {
			defer group.Done()
			summaries[position] = r.describe(ctx, state.Indices[name]).IndexSummary
		}()
	}

	group.Wait()

	return summaries, nil
}

func (r *Router) DescribeIndex(ctx context.Context, index string) (api.IndexDetail, error) {
	meta, err := r.indexMeta(index)
	if err != nil {
		return api.IndexDetail{}, err
	}

	return r.describe(ctx, meta), nil
}

func (r *Router) describe(ctx context.Context, meta *cluster.IndexMeta) api.IndexDetail {
	details := make([]api.ShardDetail, len(meta.Shards))

	var group sync.WaitGroup

	for position, shard := range meta.Shards {
		group.Add(1)

		go func() {
			defer group.Done()
			details[position] = r.shardDetail(ctx, shard)
		}()
	}

	group.Wait()

	return api.IndexDetail{IndexSummary: summarize(meta, details), Shards: details}
}

func summarize(meta *cluster.IndexMeta, details []api.ShardDetail) api.IndexSummary {
	summary := api.IndexSummary{Name: meta.Name, ShardCount: len(meta.Shards), Health: "healthy", Mappings: make([]api.FieldMapping, 0, len(meta.Mappings))}

	for _, mapping := range meta.Mappings {
		summary.Mappings = append(summary.Mappings, api.FieldMapping{Field: mapping.Field, Type: mapping.Type, VectorDim: mapping.VectorDim})
	}

	if len(meta.Shards) > 0 {
		summary.ReplicaCount = len(meta.Shards[0].Replicas)
	}

	for _, detail := range details {
		summary.DocCount += detail.DocCount

		if !detail.PrimaryReachable {
			summary.Health = "degraded"
		}
	}

	return summary
}

func (r *Router) shardDetail(ctx context.Context, shard cluster.ShardMeta) api.ShardDetail {
	detail := api.ShardDetail{ShardID: shard.ShardID, Primary: shard.Primary, Replicas: shard.Replicas}

	client, err := r.pool.Get(shard.Primary)
	if err != nil {
		return detail
	}

	callCtx, cancel := context.WithTimeout(ctx, inspectTimeout)
	defer cancel()

	status, err := client.API().ShardStatus(callCtx, &genproto.ShardStatusRequest{ShardId: shard.ShardID})
	if err != nil || !status.GetExists() {
		return detail
	}

	detail.PrimaryReachable = true
	detail.DocCount, detail.DeletedCount = status.GetDocCount(), status.GetDeletedCount()
	detail.SegmentCount, detail.WALSizeBytes, detail.MemtableDocCount = status.GetSegmentCount(), status.GetWalSizeBytes(), status.GetMemtableDocCount()

	return detail
}

// Nodes reports raft membership plus a live Health probe of every registered engine node.
func (r *Router) Nodes(ctx context.Context) (api.NodesResponse, error) {
	state := r.node.FSM.State()
	peers, _ := r.node.Peers()

	shardsPerEngine := map[string]int{}

	for _, meta := range state.Indices {
		for _, shard := range meta.Shards {
			shardsPerEngine[shard.Primary]++
		}
	}

	engines := make([]api.EngineNodeStatus, len(state.EngineNodes))

	var group sync.WaitGroup

	for position, addr := range state.EngineNodes {
		group.Add(1)

		go func() {
			defer group.Done()
			engines[position] = r.probeEngine(ctx, addr, shardsPerEngine[addr])
		}()
	}

	group.Wait()

	return api.NodesResponse{NodeID: r.node.NodeID, IsLeader: r.node.IsLeader(), Leader: r.node.LeaderAddr(), Coordinators: peers, Engines: engines}, nil
}

func (r *Router) probeEngine(ctx context.Context, addr string, shardCount int) api.EngineNodeStatus {
	status := api.EngineNodeStatus{Addr: addr, ShardCount: shardCount}

	client, err := r.pool.Get(addr)
	if err != nil {
		status.Error = err.Error()
		return status
	}

	callCtx, cancel := context.WithTimeout(ctx, inspectTimeout)
	defer cancel()

	health, err := client.API().Health(callCtx, &genproto.HealthRequest{})
	if err != nil {
		status.Error = err.Error()
		return status
	}

	status.Healthy, status.NodeID, status.UptimeSeconds = health.GetHealthy(), health.GetNodeId(), health.GetUptimeSeconds()

	return status
}

// MaintainIndex runs a flush or merge on the primary of every shard of the index.
func (r *Router) MaintainIndex(ctx context.Context, index string, action string) (api.MaintenanceResult, error) {
	meta, err := r.indexMeta(index)
	if err != nil {
		return api.MaintenanceResult{}, err
	}

	result := api.MaintenanceResult{Action: action}

	for _, shard := range meta.Shards {
		if shardErr := r.maintainShard(ctx, shard, action); shardErr != nil {
			result.ShardErrors = append(result.ShardErrors, fmt.Sprintf("%s: %v", shard.ShardID, shardErr))
			continue
		}

		result.ShardsOK++
	}

	return result, nil
}

func (r *Router) maintainShard(ctx context.Context, shard cluster.ShardMeta, action string) error {
	client, err := r.pool.Get(shard.Primary)
	if err != nil {
		return err
	}

	callCtx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()

	if action == "flush" {
		reply, err := client.API().Flush(callCtx, &genproto.FlushRequest{ShardId: shard.ShardID})
		return replyError(err, reply.GetSuccess(), reply.GetError())
	}

	reply, err := client.API().Merge(callCtx, &genproto.MergeRequest{ShardId: shard.ShardID})

	return replyError(err, reply.GetSuccess(), reply.GetError())
}

func replyError(callErr error, success bool, message string) error {
	if callErr != nil {
		return callErr
	}

	if !success {
		return fmt.Errorf("%s", message)
	}

	return nil
}

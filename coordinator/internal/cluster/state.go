// Package cluster implements the coordinator's Raft-backed cluster state:
// which indices exist, how each index's documents are partitioned into
// shards, and which engine (C++ data node) gRPC address owns each shard's
// primary copy and which addresses hold replica copies. This state is
// replicated across all coordinator nodes via the Raft consensus protocol
// (github.com/hashicorp/raft) so every coordinator has a consistent view of
// the cluster and a new leader can be elected automatically if the current
// leader crashes.
package cluster

// FieldMapping mirrors api.FieldMapping structurally (duplicated here rather
// than imported, to keep `cluster` free of a dependency on the HTTP layer —
// this package is lower-level than `api` and `routing` translates between
// the two at the boundary).
type FieldMapping struct {
	Field     string `json:"field"`
	Type      string `json:"type"`
	VectorDim int    `json:"vector_dim,omitempty"`
}

// ShardMeta describes one shard of an index: which engine node holds the
// authoritative (primary) copy, and which engine nodes hold replica copies
// kept in sync via asynchronous fan-out (see internal/replication).
type ShardMeta struct {
	ShardID  string   `json:"shard_id"`
	Index    string   `json:"index"`
	Primary  string   `json:"primary"`
	Replicas []string `json:"replicas"`
}

// IndexMeta describes one index: its field mappings and its full shard list.
type IndexMeta struct {
	Name     string         `json:"name"`
	Mappings []FieldMapping `json:"mappings"`
	Shards   []ShardMeta    `json:"shards"`
}

// State is the full replicated cluster state, applied to and read from the
// Raft FSM. All mutation happens through Command application (see fsm.go);
// callers should treat a *State obtained from FSM.State() as a read-only
// snapshot (it is a deep copy).
type State struct {
	Indices     map[string]*IndexMeta `json:"indices"`
	EngineNodes []string              `json:"engine_nodes"`
}

func newEmptyState() *State {
	return &State{Indices: make(map[string]*IndexMeta), EngineNodes: nil}
}

func (s *State) clone() *State {
	out := newEmptyState()
	out.EngineNodes = append([]string(nil), s.EngineNodes...)
	for name, idx := range s.Indices {
		clonedIdx := &IndexMeta{
			Name:     idx.Name,
			Mappings: append([]FieldMapping(nil), idx.Mappings...),
			Shards:   make([]ShardMeta, len(idx.Shards)),
		}
		for i, sh := range idx.Shards {
			clonedIdx.Shards[i] = ShardMeta{
				ShardID:  sh.ShardID,
				Index:    sh.Index,
				Primary:  sh.Primary,
				Replicas: append([]string(nil), sh.Replicas...),
			}
		}
		out.Indices[name] = clonedIdx
	}
	return out
}

// containsString reports whether needle is present in haystack.
func containsString(haystack []string, needle string) bool {
	for _, s := range haystack {
		if s == needle {
			return true
		}
	}
	return false
}

package routing

import (
	"context"

	"dse/coordinator/internal/api"
)

// ClusterStatus reports this coordinator node's raft role plus the full
// cluster metadata (known peers and every index's shard placement).
func (r *Router) ClusterStatus(ctx context.Context) (api.ClusterStatusResponse, error) {
	state := r.node.FSM.State()

	peers, err := r.node.Peers()
	if err != nil {
		peers = nil // best-effort — an unavailable configuration read shouldn't fail the whole status call
	}

	var shards []api.ShardInfo
	for _, idx := range state.Indices {
		for _, sh := range idx.Shards {
			shards = append(shards, api.ShardInfo{
				ShardID:  sh.ShardID,
				Index:    sh.Index,
				Primary:  sh.Primary,
				Replicas: sh.Replicas,
			})
		}
	}

	return api.ClusterStatusResponse{
		NodeID:   r.node.NodeID,
		IsLeader: r.node.IsLeader(),
		Leader:   r.node.LeaderAddr(),
		Nodes:    peers,
		Shards:   shards,
	}, nil
}

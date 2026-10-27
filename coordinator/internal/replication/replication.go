// Package replication fans a write out from a shard's primary engine node to
// its replica engine nodes, so replicas eventually converge on the same
// document set.
//
// Consistency model (deliberately simple — see DOCUMENTATION.md
// "Replication consistency" for the full discussion): writes are applied
// SYNCHRONOUSLY to the primary (the caller waits for and returns the
// primary's response) and ASYNCHRONOUSLY, best-effort, fire-and-forget to
// each replica afterward. This gives read-your-writes consistency against
// the primary but only EVENTUAL consistency on replicas — a real system
// would want synchronous quorum writes or a per-shard consensus/replication
// log (e.g. treating each shard itself as a small raft group) for strict
// guarantees. This package is intentionally the single place that tradeoff
// lives, so it can be swapped out later without touching routing logic.
package replication

import (
	"context"
	"log/slog"
	"time"

	"dse/coordinator/internal/cluster"
	"dse/coordinator/internal/engineclient"
	"dse/coordinator/internal/genproto"
)

const asyncReplicaTimeout = 5 * time.Second

// IndexDocument sends the index request to shard.Primary and returns its
// response synchronously, then fans the same request out to shard.Replicas
// in the background.
func IndexDocument(ctx context.Context, pool *engineclient.Pool, shard cluster.ShardMeta,
	req *genproto.IndexRequest) (*genproto.IndexResponse, error) {
	primary, err := pool.Get(shard.Primary)
	if err != nil {
		return nil, err
	}
	resp, err := primary.API().IndexDocument(ctx, req)
	if err != nil {
		return nil, err
	}
	fanOutAsync(pool, shard.Replicas, func(c *engineclient.Client) {
		rctx, cancel := context.WithTimeout(context.Background(), asyncReplicaTimeout)
		defer cancel()
		if _, err := c.API().IndexDocument(rctx, req); err != nil {
			slog.Warn("replica index failed", "replica", c.Addr, "shard", shard.ShardID, "err", err)
		}
	})
	return resp, nil
}

// DeleteDocument mirrors IndexDocument for deletes.
func DeleteDocument(ctx context.Context, pool *engineclient.Pool, shard cluster.ShardMeta,
	req *genproto.DeleteRequest) (*genproto.DeleteResponse, error) {
	primary, err := pool.Get(shard.Primary)
	if err != nil {
		return nil, err
	}
	resp, err := primary.API().DeleteDocument(ctx, req)
	if err != nil {
		return nil, err
	}
	fanOutAsync(pool, shard.Replicas, func(c *engineclient.Client) {
		rctx, cancel := context.WithTimeout(context.Background(), asyncReplicaTimeout)
		defer cancel()
		if _, err := c.API().DeleteDocument(rctx, req); err != nil {
			slog.Warn("replica delete failed", "replica", c.Addr, "shard", shard.ShardID, "err", err)
		}
	})
	return resp, nil
}

// BulkIndex mirrors IndexDocument for a batch of ops targeting one shard.
func BulkIndex(ctx context.Context, pool *engineclient.Pool, shard cluster.ShardMeta,
	req *genproto.BulkRequest) (*genproto.BulkResponse, error) {
	primary, err := pool.Get(shard.Primary)
	if err != nil {
		return nil, err
	}
	resp, err := primary.API().BulkIndex(ctx, req)
	if err != nil {
		return nil, err
	}
	fanOutAsync(pool, shard.Replicas, func(c *engineclient.Client) {
		rctx, cancel := context.WithTimeout(context.Background(), asyncReplicaTimeout)
		defer cancel()
		if _, err := c.API().BulkIndex(rctx, req); err != nil {
			slog.Warn("replica bulk index failed", "replica", c.Addr, "shard", shard.ShardID, "err", err)
		}
	})
	return resp, nil
}

// CreateShard is sent SYNCHRONOUSLY to the primary AND every replica (unlike
// document writes) because shard creation is a one-time, low-frequency
// administrative operation where we want to surface failures immediately
// rather than discover a missing replica shard later during a document
// write.
func CreateShard(ctx context.Context, pool *engineclient.Pool, shard cluster.ShardMeta,
	req *genproto.CreateShardRequest) error {
	targets := append([]string{shard.Primary}, shard.Replicas...)
	for _, addr := range targets {
		c, err := pool.Get(addr)
		if err != nil {
			return err
		}
		resp, err := c.API().CreateShard(ctx, req)
		if err != nil {
			return err
		}
		if !resp.GetSuccess() {
			return &CreateShardError{Addr: addr, Message: resp.GetError()}
		}
	}
	return nil
}

// CreateShardError reports which engine node rejected a CreateShard call.
type CreateShardError struct {
	Addr    string
	Message string
}

func (e *CreateShardError) Error() string {
	return "create shard failed on " + e.Addr + ": " + e.Message
}

func fanOutAsync(pool *engineclient.Pool, addrs []string, fn func(*engineclient.Client)) {
	for _, addr := range addrs {
		c, err := pool.Get(addr)
		if err != nil {
			slog.Warn("replica unreachable", "addr", addr, "err", err)
			continue
		}
		go fn(c)
	}
}

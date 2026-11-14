package api

import "context"

// IndexSummary is the list-view projection of an index.
type IndexSummary struct {
	Name         string         `json:"name"`
	Mappings     []FieldMapping `json:"mappings"`
	ShardCount   int            `json:"shard_count"`
	ReplicaCount int            `json:"replica_count"`
	DocCount     int64          `json:"doc_count"`
	Health       string         `json:"health"` // "healthy" when every primary answers, else "degraded"
}

// ShardDetail is the live status of one shard as reported by its primary engine node.
type ShardDetail struct {
	ShardID          string   `json:"shard_id"`
	Primary          string   `json:"primary"`
	Replicas         []string `json:"replicas"`
	PrimaryReachable bool     `json:"primary_reachable"`
	DocCount         int64    `json:"doc_count"`
	DeletedCount     int64    `json:"deleted_count"`
	SegmentCount     int32    `json:"segment_count"`
	WALSizeBytes     int64    `json:"wal_size_bytes"`
	MemtableDocCount int64    `json:"memtable_doc_count"`
}

type IndexDetail struct {
	IndexSummary
	Shards []ShardDetail `json:"shards"`
}

type EngineNodeStatus struct {
	Addr          string `json:"addr"`
	NodeID        string `json:"node_id"`
	Healthy       bool   `json:"healthy"`
	UptimeSeconds int64  `json:"uptime_seconds"`
	ShardCount    int    `json:"shard_count"`
	Error         string `json:"error,omitempty"`
}

type NodesResponse struct {
	NodeID       string             `json:"node_id"`
	IsLeader     bool               `json:"is_leader"`
	Leader       string             `json:"leader"`
	Coordinators []string           `json:"coordinators"`
	Engines      []EngineNodeStatus `json:"engines"`
}

type MaintenanceResult struct {
	Action      string   `json:"action"`
	ShardsOK    int      `json:"shards_ok"`
	ShardErrors []string `json:"shard_errors,omitempty"`
}

// Inspector exposes read/ops views of the cluster for the web console; implemented by the routing package.
type Inspector interface {
	ListIndices(ctx context.Context) ([]IndexSummary, error)
	DescribeIndex(ctx context.Context, index string) (IndexDetail, error)
	Nodes(ctx context.Context) (NodesResponse, error)
	MaintainIndex(ctx context.Context, index string, action string) (MaintenanceResult, error)
}

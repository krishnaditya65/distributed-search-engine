// Package api implements the REST API layer of the distributed search
// engine coordinator. It defines the JSON wire types and the Coordinator
// interface the HTTP handlers depend on; the actual search/indexing logic
// is implemented elsewhere (see the routing package) against this contract.
package api

import "encoding/json"

// BulkOp is a single operation within a bulk request.
type BulkOp struct {
	Type  string          `json:"type"` // "index" | "delete"
	DocID string          `json:"doc_id,omitempty"`
	Body  json.RawMessage `json:"body,omitempty"` // required for "index"
}

// BulkOpResult is the outcome of a single BulkOp.
type BulkOpResult struct {
	Success bool   `json:"success"`
	Error   string `json:"error,omitempty"`
	DocID   string `json:"doc_id"`
}

// FieldMapping describes how a single field should be indexed.
type FieldMapping struct {
	Field     string `json:"field"`
	Type      string `json:"type"` // "text" | "keyword" | "number" | "vector"
	VectorDim int    `json:"vector_dim,omitempty"`
}

// CreateIndexRequest is the body of PUT /indices/{index}.
type CreateIndexRequest struct {
	Mappings []FieldMapping `json:"mappings"`
	Shards   int            `json:"shards,omitempty"`
	Replicas int            `json:"replicas,omitempty"`
}

// TermFilter matches documents where Field == Value exactly.
type TermFilter struct {
	Field string `json:"field"`
	Value string `json:"value"`
}

// RangeFilter matches documents where Field falls within [Gte, Lte].
type RangeFilter struct {
	Field string   `json:"field"`
	Gte   *float64 `json:"gte,omitempty"`
	Lte   *float64 `json:"lte,omitempty"`
}

// Filter groups term and range filters, combined with AND semantics.
type Filter struct {
	Term  []TermFilter  `json:"term,omitempty"`
	Range []RangeFilter `json:"range,omitempty"`
}

// VectorQuery requests a k-nearest-neighbor search over a vector field.
type VectorQuery struct {
	Field     string    `json:"field"`
	Embedding []float32 `json:"embedding"`
	K         int       `json:"k"`
}

// Aggregation requests a bucketed or statistical aggregation over a field.
type Aggregation struct {
	Name     string  `json:"name"`
	Type     string  `json:"type"` // "terms" | "stats" | "histogram"
	Field    string  `json:"field"`
	Size     int     `json:"size,omitempty"`
	Interval float64 `json:"interval,omitempty"`
}

// SearchRequest is the body of POST /indices/{index}/_search.
type SearchRequest struct {
	Query        string        `json:"query,omitempty"`
	Fields       []string      `json:"fields,omitempty"`
	Filter       *Filter       `json:"filter,omitempty"`
	Vector       *VectorQuery  `json:"vector,omitempty"`
	VectorWeight float64       `json:"vector_weight,omitempty"`
	Aggregations []Aggregation `json:"aggregations,omitempty"`
	Size         int           `json:"size,omitempty"`
	From         int           `json:"from,omitempty"`
}

// SearchHit is a single matched document.
type SearchHit struct {
	DocID  string          `json:"doc_id"`
	Score  float64         `json:"score"`
	Source json.RawMessage `json:"source"`
}

// Bucket is a single terms/histogram aggregation bucket.
type Bucket struct {
	Key      string `json:"key"`
	DocCount int64  `json:"doc_count"`
}

// StatsResult holds the result of a "stats" aggregation.
type StatsResult struct {
	Count int64   `json:"count"`
	Sum   float64 `json:"sum"`
	Min   float64 `json:"min"`
	Max   float64 `json:"max"`
	Avg   float64 `json:"avg"`
}

// AggregationResult is the result of a single requested Aggregation.
type AggregationResult struct {
	Name    string       `json:"name"`
	Buckets []Bucket     `json:"buckets,omitempty"`
	Stats   *StatsResult `json:"stats,omitempty"`
}

// SearchResponse is the body returned by POST /indices/{index}/_search.
type SearchResponse struct {
	Hits         []SearchHit         `json:"hits"`
	TotalHits    int64               `json:"total_hits"`
	Aggregations []AggregationResult `json:"aggregations,omitempty"`
	TookMillis   int64               `json:"took_millis"`
}

// ShardInfo describes the placement of a single shard.
type ShardInfo struct {
	ShardID  string   `json:"shard_id"`
	Index    string   `json:"index"`
	Primary  string   `json:"primary"`
	Replicas []string `json:"replicas"`
}

// ClusterStatusResponse is the body returned by GET /_cluster/health.
type ClusterStatusResponse struct {
	NodeID   string      `json:"node_id"`
	IsLeader bool        `json:"is_leader"`
	Leader   string      `json:"leader"`
	Nodes    []string    `json:"nodes"`
	Shards   []ShardInfo `json:"shards"`
}

package routing

import (
	"testing"

	"dse/coordinator/internal/api"
	"dse/coordinator/internal/genproto"
)

func TestToProtoSearchRequestPreservesFiltersVectorAndGlobalPaging(t *testing.T) {
	gte, lte := 10.0, 99.0
	req := api.SearchRequest{
		Query: "running shoes", Fields: []string{"title"}, From: 4, Size: 3,
		Filter: &api.Filter{
			Term:  []api.TermFilter{{Field: "brand", Value: "Nike"}},
			Range: []api.RangeFilter{{Field: "price", Gte: &gte, Lte: &lte}},
		},
		Vector:       &api.VectorQuery{Field: "embedding", Embedding: []float32{1, 0, 0}, K: 5},
		VectorWeight: 0.4,
		Aggregations: []api.Aggregation{{Name: "brands", Type: "terms", Field: "brand", Size: 2}},
	}
	pb := toProtoSearchRequest(req, 3)
	if pb.GetQueryText() != req.Query || pb.GetSize() != 7 || pb.GetFrom() != 0 {
		t.Fatalf("unexpected query paging: query=%q size=%d from=%d", pb.GetQueryText(), pb.GetSize(), pb.GetFrom())
	}
	if len(pb.GetFilter().GetTerm()) != 1 || pb.GetFilter().GetTerm()[0].GetValue() != "Nike" {
		t.Fatalf("term filter was not translated: %v", pb.GetFilter())
	}
	if pb.GetFilter().GetRange()[0].GetGte() != gte || pb.GetFilter().GetRange()[0].GetLte() != lte {
		t.Fatalf("range filter was not translated: %v", pb.GetFilter().GetRange()[0])
	}
	if pb.GetVector().GetField() != "embedding" || pb.GetVector().GetK() != 5 || pb.GetVectorWeight() != 0.4 {
		t.Fatalf("vector query was not translated: %v", pb.GetVector())
	}
	if pb.GetAggregations()[0].GetSize() != 16 { // requested size*3 + 10 over-fetch
		t.Fatalf("unexpected aggregation over-fetch size: %d", pb.GetAggregations()[0].GetSize())
	}
}

func TestMergeAggregationsCombinesAndSortsShardResults(t *testing.T) {
	requested := []api.Aggregation{
		{Name: "brand", Type: "terms", Field: "brand", Size: 2},
		{Name: "price", Type: "stats", Field: "price"},
		{Name: "hist", Type: "histogram", Field: "price", Interval: 10},
	}
	perShard := [][]*genproto.AggregationResult{{
		{Name: "brand", Buckets: []*genproto.Bucket{{Key: "Nike", DocCount: 2}, {Key: "Adidas", DocCount: 1}}},
		{Name: "price", Stats: &genproto.StatsResult{Count: 2, Sum: 30, Min: 10, Max: 20}},
		{Name: "hist", Buckets: []*genproto.Bucket{{Key: "10", DocCount: 2}}},
	}, {
		{Name: "brand", Buckets: []*genproto.Bucket{{Key: "Nike", DocCount: 1}, {Key: "Puma", DocCount: 8}}},
		{Name: "price", Stats: &genproto.StatsResult{Count: 1, Sum: 40, Min: 40, Max: 40}},
		{Name: "hist", Buckets: []*genproto.Bucket{{Key: "2", DocCount: 1}, {Key: "10", DocCount: 1}}},
	}}
	merged := mergeAggregations(requested, perShard)
	if len(merged) != 3 || merged[0].Buckets[0].Key != "Puma" || merged[0].Buckets[0].DocCount != 8 || merged[0].Buckets[1].Key != "Nike" {
		t.Fatalf("unexpected terms merge: %#v", merged[0])
	}
	if merged[1].Stats.Count != 3 || merged[1].Stats.Sum != 70 || merged[1].Stats.Min != 10 || merged[1].Stats.Max != 40 || merged[1].Stats.Avg != 70.0/3.0 {
		t.Fatalf("unexpected stats merge: %#v", merged[1].Stats)
	}
	if len(merged[2].Buckets) != 2 || merged[2].Buckets[0].Key != "2" || merged[2].Buckets[1].DocCount != 3 {
		t.Fatalf("unexpected histogram merge: %#v", merged[2].Buckets)
	}
	if !numericKeyLess("2", "10") || numericKeyLess("10", "2") || !numericKeyLess("bad", "zz") {
		t.Fatal("numeric bucket ordering edge cases failed")
	}
}

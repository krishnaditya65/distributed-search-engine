package routing

import (
	"context"
	"encoding/json"
	"sort"
	"strconv"
	"sync"
	"time"

	"google.golang.org/protobuf/proto"

	"dse/coordinator/internal/api"
	"dse/coordinator/internal/genproto"
)

// Search implements the scatter-gather query path: build one gRPC
// SearchRequest per shard, fan them out to every shard's primary IN
// PARALLEL, wait for all of them, then merge hits (globally re-sorted and
// paginated) and aggregations (see mergeAggregations) into a single
// api.SearchResponse.
func (r *Router) Search(ctx context.Context, index string, req api.SearchRequest) (api.SearchResponse, error) {
	start := time.Now()
	meta, err := r.indexMeta(index)
	if err != nil {
		return api.SearchResponse{}, err
	}

	pbReq := toProtoSearchRequest(req, len(meta.Shards))

	type shardResult struct {
		resp *genproto.SearchResponse
		err  error
	}
	results := make([]shardResult, len(meta.Shards))

	var wg sync.WaitGroup
	for i, shard := range meta.Shards {
		wg.Add(1)
		go func(i int, shardID, primary string) {
			defer wg.Done()
			client, err := r.pool.Get(primary)
			if err != nil {
				results[i] = shardResult{err: err}
				return
			}
			reqCopy := proto.Clone(pbReq).(*genproto.SearchRequest)
			reqCopy.ShardId = shardID
			resp, err := client.API().Search(ctx, reqCopy)
			results[i] = shardResult{resp: resp, err: err}
		}(i, shard.ShardID, shard.Primary)
	}
	wg.Wait()

	var allHits []*genproto.SearchHit
	var totalHits int64
	perShardAggs := make([][]*genproto.AggregationResult, 0, len(results))
	for _, result := range results {
		if result.err != nil || result.resp == nil || !result.resp.GetSuccess() {
			// A single unreachable/failed shard degrades the result set
			// (missing that shard's documents) rather than failing the
			// whole query — documented "partial results on shard failure"
			// behavior, matching how most distributed search engines
			// behave by default (Elasticsearch calls this
			// "_shards.failed > 0" but still returns a 200).
			continue
		}
		allHits = append(allHits, result.resp.GetHits()...)
		totalHits += result.resp.GetTotalHits()
		perShardAggs = append(perShardAggs, result.resp.GetAggregations())
	}

	sort.Slice(allHits, func(i, j int) bool {
		if allHits[i].GetScore() != allHits[j].GetScore() {
			return allHits[i].GetScore() > allHits[j].GetScore()
		}
		return allHits[i].GetDocId() < allHits[j].GetDocId()
	})

	from := req.From
	size := req.Size
	if size <= 0 {
		size = 10
	}
	end := from + size
	if end > len(allHits) {
		end = len(allHits)
	}
	if from > len(allHits) {
		from = len(allHits)
	}

	out := api.SearchResponse{TotalHits: totalHits}
	for _, h := range allHits[from:end] {
		out.Hits = append(out.Hits, api.SearchHit{
			DocID:  h.GetDocId(),
			Score:  h.GetScore(),
			Source: json.RawMessage(h.GetJson()),
		})
	}
	out.Aggregations = mergeAggregations(req.Aggregations, perShardAggs)
	out.TookMillis = time.Since(start).Milliseconds()
	return out, nil
}

func toProtoSearchRequest(req api.SearchRequest, numShards int) *genproto.SearchRequest {
	pbReq := &genproto.SearchRequest{
		QueryText:    req.Query,
		Fields:       req.Fields,
		VectorWeight: req.VectorWeight,
		From:         0, // per-shard offset is always 0; global pagination is applied after merge
	}

	// Request enough candidates from EACH shard to correctly compute the
	// GLOBAL top (From+Size) page — since we don't know in advance how many
	// of the top results live on any particular shard.
	perShardSize := req.From + req.Size
	if perShardSize <= 0 {
		perShardSize = 10
	}
	pbReq.Size = int32(perShardSize)

	if req.Filter != nil {
		filter := &genproto.Filter{}
		for _, term := range req.Filter.Term {
			filter.Term = append(filter.Term, &genproto.TermFilter{Field: term.Field, Value: term.Value})
		}
		for _, rng := range req.Filter.Range {
			rangeFilter := &genproto.RangeFilter{Field: rng.Field}
			if rng.Gte != nil {
				v := *rng.Gte
				rangeFilter.Gte = &v
			}
			if rng.Lte != nil {
				v := *rng.Lte
				rangeFilter.Lte = &v
			}
			filter.Range = append(filter.Range, rangeFilter)
		}
		pbReq.Filter = filter
	}

	if req.Vector != nil {
		pbReq.Vector = &genproto.VectorQuery{
			Field:     req.Vector.Field,
			Embedding: req.Vector.Embedding,
			K:         int32(req.Vector.K),
		}
	}

	for _, agg := range req.Aggregations {
		pbAgg := &genproto.Aggregation{
			Name:     agg.Name,
			Type:     agg.Type,
			Field:    agg.Field,
			Interval: agg.Interval,
		}
		// Over-fetch terms/histogram buckets per shard so the merged,
		// globally re-ranked top-N is more accurate — the same mitigation
		// real distributed search engines use for approximate distributed
		// terms aggregations (see DOCUMENTATION.md "Distributed
		// aggregations").
		if agg.Size > 0 {
			pbAgg.Size = int32(agg.Size*3 + 10)
		} else {
			pbAgg.Size = 40
		}
		pbReq.Aggregations = append(pbReq.Aggregations, pbAgg)
	}

	_ = numShards // reserved: a future optimization could special-case numShards==1 to skip merging
	return pbReq
}

// mergeAggregations combines one AggregationResult list per shard into a
// single globally-correct (or, for "terms", best-effort-approximate — see
// comment above) result per originally-requested aggregation name.
func mergeAggregations(requested []api.Aggregation, perShard [][]*genproto.AggregationResult) []api.AggregationResult {
	if len(requested) == 0 {
		return nil
	}

	// name -> requested spec, to know target bucket size and type for the
	// final truncation step.
	specByName := make(map[string]api.Aggregation, len(requested))
	order := make([]string, 0, len(requested))
	for _, a := range requested {
		specByName[a.Name] = a
		order = append(order, a.Name)
	}

	termBuckets := make(map[string]map[string]*bucketAcc) // agg name -> key -> acc
	histBuckets := make(map[string]map[string]*bucketAcc) // agg name -> numeric-string key -> acc
	statsAcc := make(map[string]*api.StatsResult)

	for _, shardAggs := range perShard {
		for _, shardAgg := range shardAggs {
			spec, ok := specByName[shardAgg.GetName()]
			if !ok {
				continue
			}
			switch spec.Type {
			case "terms":
				bucketMap, ok := termBuckets[shardAgg.GetName()]
				if !ok {
					bucketMap = make(map[string]*bucketAcc)
					termBuckets[shardAgg.GetName()] = bucketMap
				}
				for _, b := range shardAgg.GetBuckets() {
					acc, ok := bucketMap[b.GetKey()]
					if !ok {
						acc = &bucketAcc{}
						bucketMap[b.GetKey()] = acc
					}
					acc.count += b.GetDocCount()
				}
			case "histogram":
				bucketMap, ok := histBuckets[shardAgg.GetName()]
				if !ok {
					bucketMap = make(map[string]*bucketAcc)
					histBuckets[shardAgg.GetName()] = bucketMap
				}
				for _, b := range shardAgg.GetBuckets() {
					acc, ok := bucketMap[b.GetKey()]
					if !ok {
						acc = &bucketAcc{}
						bucketMap[b.GetKey()] = acc
					}
					acc.count += b.GetDocCount()
				}
			case "stats":
				acc, ok := statsAcc[shardAgg.GetName()]
				if !ok {
					acc = &api.StatsResult{Min: shardAgg.GetStats().GetMin(), Max: shardAgg.GetStats().GetMax()}
					statsAcc[shardAgg.GetName()] = acc
				}
				shardStats := shardAgg.GetStats()
				acc.Count += shardStats.GetCount()
				acc.Sum += shardStats.GetSum()
				if shardStats.GetCount() > 0 {
					if shardStats.GetMin() < acc.Min || acc.Count == shardStats.GetCount() {
						acc.Min = minF(acc.Min, shardStats.GetMin())
					}
					acc.Max = maxF(acc.Max, shardStats.GetMax())
				}
			}
		}
	}

	out := make([]api.AggregationResult, 0, len(requested))
	for _, name := range order {
		spec := specByName[name]
		switch spec.Type {
		case "terms":
			buckets := flattenBuckets(termBuckets[name])
			sort.Slice(buckets, func(i, j int) bool {
				if buckets[i].DocCount != buckets[j].DocCount {
					return buckets[i].DocCount > buckets[j].DocCount
				}
				return buckets[i].Key < buckets[j].Key
			})
			size := spec.Size
			if size <= 0 {
				size = 10
			}
			if len(buckets) > size {
				buckets = buckets[:size]
			}
			out = append(out, api.AggregationResult{Name: name, Buckets: buckets})
		case "histogram":
			buckets := flattenBuckets(histBuckets[name])
			sort.Slice(buckets, func(i, j int) bool {
				return numericKeyLess(buckets[i].Key, buckets[j].Key)
			})
			out = append(out, api.AggregationResult{Name: name, Buckets: buckets})
		case "stats":
			s := statsAcc[name]
			if s == nil {
				s = &api.StatsResult{}
			} else if s.Count > 0 {
				s.Avg = s.Sum / float64(s.Count)
			}
			out = append(out, api.AggregationResult{Name: name, Stats: s})
		}
	}
	return out
}

// bucketAcc accumulates a single bucket's merged doc_count across shards.
type bucketAcc struct {
	count int64
}

func flattenBuckets(m map[string]*bucketAcc) []api.Bucket {
	out := make([]api.Bucket, 0, len(m))
	for k, v := range m {
		out = append(out, api.Bucket{Key: k, DocCount: v.count})
	}
	return out
}

func minF(a, b float64) float64 {
	if a < b {
		return a
	}
	return b
}

func maxF(a, b float64) float64 {
	if a > b {
		return a
	}
	return b
}

// numericKeyLess compares two histogram bucket keys (formatted numbers, e.g.
// "0", "5", "10") numerically rather than lexicographically, so "10" sorts
// after "5" instead of before it.
func numericKeyLess(a, b string) bool {
	af, aErr := strconv.ParseFloat(a, 64)
	bf, bErr := strconv.ParseFloat(b, 64)
	if aErr == nil && bErr == nil {
		return af < bf
	}
	return a < b
}

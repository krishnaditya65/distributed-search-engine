#!/usr/bin/env bash
# demo.sh — walks through every major feature of the engine against a
# cluster started by run_cluster.sh: index creation with sharding +
# replication, document CRUD, bulk indexing, BM25 keyword search, term/range
# filters, distributed aggregations (terms/stats/histogram), and hybrid
# vector search. Also demonstrates crash recovery by hard-killing and
# restarting engine-1 mid-demo.
set -euo pipefail
BASE=http://127.0.0.1:8090
DATA_DIR=/tmp/dse_cluster
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

hr() { printf '\n\033[1;36m=== %s ===\033[0m\n' "$1"; }
pp() { python3 -m json.tool 2>/dev/null || cat; }

if ! curl -sf "$BASE/health" > /dev/null; then
  echo "Cluster not reachable at $BASE — run ./scripts/run_cluster.sh first." >&2
  exit 1
fi

hr "1. Create an index with text/keyword/number/vector field mappings, 3 shards, 1 replica"
curl -s -X PUT "$BASE/indices/products" -d '{
  "mappings": [
    {"field":"title","type":"text"},
    {"field":"brand","type":"keyword"},
    {"field":"price","type":"number"},
    {"field":"embedding","type":"vector","vector_dim":4}
  ],
  "shards": 3,
  "replicas": 1
}' | pp
sleep 1

hr "2. Cluster health — shard placement across engine nodes (sharding + replication)"
curl -s "$BASE/_cluster/health" | pp

hr "3. Bulk index 5 documents"
curl -s -X POST "$BASE/indices/products/_bulk" -d '{
  "ops": [
    {"type":"index","doc_id":"p1","body":{"title":"Red Running Shoes","brand":"Nike","price":89,"embedding":[1,0,0,0]}},
    {"type":"index","doc_id":"p2","body":{"title":"Blue Running Shoes","brand":"Nike","price":95,"embedding":[0.9,0.1,0,0]}},
    {"type":"index","doc_id":"p3","body":{"title":"Leather Boots","brand":"Adidas","price":150,"embedding":[0,1,0,0]}},
    {"type":"index","doc_id":"p4","body":{"title":"Trail Running Shoes","brand":"Adidas","price":120,"embedding":[0.8,0.2,0,0]}},
    {"type":"index","doc_id":"p5","body":{"title":"Casual Sneakers","brand":"Puma","price":60,"embedding":[0,0,1,0]}}
  ]
}' | pp
sleep 1

hr "4. GET a single document (routed to its owning shard)"
curl -s "$BASE/indices/products/_doc/p3" | pp

hr "5. Update a document (delete-old-internal-id + reindex, transparent to the client)"
curl -s -X PUT "$BASE/indices/products/_doc/p3" -d '{"title":"Premium Leather Boots","brand":"Adidas","price":175,"embedding":[0,1,0,0]}' | pp
curl -s "$BASE/indices/products/_doc/p3" | pp

hr "6. BM25 keyword search: 'running shoes' (scatter-gather across all shards, globally ranked)"
curl -s -X POST "$BASE/indices/products/_search" -d '{"query":"running shoes","fields":["title"],"size":10}' | pp

hr "7. Term filter: brand = Nike (uses the inverted index's exact-match postings)"
curl -s -X POST "$BASE/indices/products/_search" -d '{"filter":{"term":[{"field":"brand","value":"Nike"}]}}' | pp

hr "8. Range filter: price >= 90"
curl -s -X POST "$BASE/indices/products/_search" -d '{"filter":{"range":[{"field":"price","gte":90}]}}' | pp

hr "9. Distributed aggregations: terms by brand, stats + histogram on price"
curl -s -X POST "$BASE/indices/products/_search" -d '{
  "size": 3,
  "aggregations": [
    {"name":"by_brand","type":"terms","field":"brand","size":10},
    {"name":"price_stats","type":"stats","field":"price"},
    {"name":"price_histogram","type":"histogram","field":"price","interval":50}
  ]
}' | pp

hr "10. Hybrid search: vector similarity to [1,0,0,0] blended with keyword relevance"
curl -s -X POST "$BASE/indices/products/_search" -d '{
  "query": "shoes",
  "fields": ["title"],
  "vector": {"field":"embedding","embedding":[1,0,0,0],"k":5},
  "vector_weight": 0.5,
  "size": 5
}' | pp

hr "11. Delete a document"
curl -s -X DELETE "$BASE/indices/products/_doc/p5" | pp
curl -s "$BASE/indices/products/_doc/p5" | pp

hr "12. Crash recovery: hard-kill engine-1, verify DEGRADED (not failed) results, restart, verify full recovery"
E1_PID=$(cat "$DATA_DIR/engine1.pid")
echo "killing engine-1 (pid $E1_PID) with SIGKILL..."
kill -9 "$E1_PID" || true
sleep 1
echo "search with engine-1 down (partial results from the surviving node):"
curl -s -X POST "$BASE/indices/products/_search" -d '{"size":10}' | pp

echo "restarting engine-1..."
"$ROOT_DIR/engine/build/dse_engine" --port=7001 --data-dir="$DATA_DIR/engine1" --node-id=engine-1 \
  --maintenance-interval-sec=10 > "$DATA_DIR/engine1.log" 2>&1 &
echo $! > "$DATA_DIR/engine1.pid"
sleep 2
echo "search after engine-1 restart (WAL replay + segment reload should have recovered everything):"
curl -s -X POST "$BASE/indices/products/_search" -d '{"size":10}' | pp

hr "Demo complete"
echo "See DOCUMENTATION.md for the full architecture and feature reference."

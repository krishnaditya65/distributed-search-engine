# dse — a distributed search engine, built from scratch

A hybrid **Go (cluster/routing) + C++ (storage/execution)** distributed search
engine in the spirit of Elasticsearch: sharding, replication, scatter-gather
search, an inverted index with BM25 relevance scoring, an HNSW vector index
for hybrid semantic + keyword search, LSM-style immutable segments with
background merging, a write-ahead log with crash recovery, Roaring-Bitmap
postings, memory-mapped segment I/O, Raft-based cluster metadata, and
distributed aggregations (faceting).

Every feature listed below is real, working code — not a stub — verified by
10 C++ test binaries (100% passing under `ctest`, covering 16+ named
scenarios across analysis, bitmap, WAL, storage, scoring, vector, aggregation,
index, segment, and full shard-integration tests) plus 13 Go test functions
(API layer + config, all passing), plus a live, scripted, multi-process
end-to-end demo that includes a genuine `kill -9` crash-and-recover scenario.
See **[DOCUMENTATION.md](DOCUMENTATION.md)** for the complete architectural
deep dive — this file is just a map and a quick start.

## Quick start

Requires: Go 1.23+, CMake 3.20+, a C++17 compiler, and `protoc` + gRPC C++
(`brew install grpc protobuf` on macOS installs everything needed, including
the codegen plugins).

```bash
./scripts/build.sh          # builds + tests both the C++ engine and Go coordinator
./scripts/run_cluster.sh    # starts 2 engine nodes + 1 coordinator locally
./scripts/demo.sh           # walks through every feature via the REST API
./scripts/stop_cluster.sh   # tears it all down
```

`run_cluster.sh` starts the coordinator's REST API on `http://127.0.0.1:8090`.
Everything below is a plain HTTP call:

```bash
# create an index: 3 shards, 1 replica each, with a text/keyword/number/vector schema
curl -X PUT localhost:8090/indices/products -d '{
  "mappings": [
    {"field":"title","type":"text"},
    {"field":"brand","type":"keyword"},
    {"field":"price","type":"number"},
    {"field":"embedding","type":"vector","vector_dim":4}
  ],
  "shards": 3, "replicas": 1
}'

# index a document
curl -X PUT localhost:8090/indices/products/_doc/p1 \
  -d '{"title":"Red Running Shoes","brand":"Nike","price":89,"embedding":[1,0,0,0]}'

# BM25 keyword search
curl -X POST localhost:8090/indices/products/_search \
  -d '{"query":"running shoes","fields":["title"]}'

# hybrid keyword + vector search with a term filter and a facet
curl -X POST localhost:8090/indices/products/_search -d '{
  "query": "shoes", "fields": ["title"],
  "filter": {"term": [{"field":"brand","value":"Nike"}]},
  "vector": {"field":"embedding","embedding":[1,0,0,0],"k":5},
  "vector_weight": 0.5,
  "aggregations": [{"name":"by_brand","type":"terms","field":"brand"}]
}'
```

## What's here

```
distributed-search-engine/
├── DOCUMENTATION.md     ← the full architectural blueprint (start here for depth)
├── proto/engine.proto   ← the Go↔C++ gRPC contract (the only thing the two languages agree on)
├── coordinator/         ← Go: REST API, Raft cluster metadata, scatter-gather routing
├── engine/              ← C++: inverted index, BM25, HNSW, LSM segments, WAL, mmap storage
├── scripts/             ← build.sh, run_cluster.sh, demo.sh, stop_cluster.sh
└── examples/            ← a small sample JSONL dataset
```

## Feature checklist (against the original design brief)

**Foundational**
- [x] Inverted index (`engine/src/index/`)
- [x] Document CRUD (index/get/delete/bulk, both engine gRPC and coordinator REST)
- [x] Text analysis pipeline — tokenizer, lowercasing, stopword filtering (`engine/src/analysis/`)
- [x] Sharding — configurable shard count per index, FNV-hash-based routing (`coordinator/internal/cluster`, `internal/routing`)
- [x] Replication — primary + N replicas per shard, async fan-out writes (`coordinator/internal/replication`)
- [x] Scatter-gather search — parallel per-shard gRPC fan-out, global re-rank/merge (`coordinator/internal/routing/search.go`)

**Advanced**
- [x] Hybrid search (vector + keyword) — from-scratch HNSW graph, blended scoring (`engine/src/vector/`)
- [x] BM25 relevance scoring — smoothed IDF, term-frequency saturation, length normalization (`engine/src/scoring/`)
- [x] Immutable segments + LSM merging — memtable → flush → immutable segment → background merge (`engine/src/segment/`, `engine/src/shard/`)
- [x] Memory-mapped files + Roaring Bitmaps — real `mmap`/`munmap`/`msync`, a real array/bitset roaring bitmap with AND/OR/ANDNOT (`engine/src/storage/`, `engine/src/bitmap/`)
- [x] Write-ahead log + crash recovery — length-prefixed, CRC32-checksummed, fsync'd; replayed on restart (`engine/src/wal/`); verified with a real `kill -9` + restart in `scripts/demo.sh`
- [x] Distributed aggregations (faceting) — per-shard terms/stats/histogram, correctly merged coordinator-side (`engine/src/aggregation/`, `coordinator/internal/routing/search.go`)
- [x] Raft-based cluster metadata — `hashicorp/raft`, bootstrap + join, replicated shard-placement FSM (`coordinator/internal/cluster/`)
- [x] Background LSM maintenance thread — a real `std::thread` in the engine process periodically flushes and merges (`engine/src/main.cpp`)

Known simplifications, honest gaps, and the full "why" behind every design
decision are documented in **[DOCUMENTATION.md](DOCUMENTATION.md)**.

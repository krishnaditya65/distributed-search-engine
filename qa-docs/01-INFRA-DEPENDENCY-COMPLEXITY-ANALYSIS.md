# distributed-search-engine — Infra, Dependency & Complexity Analysis

Audience: LLM agents writing/running tests. Nothing here has been executed.

## 1. What it is
Elasticsearch-like distributed search engine in two tiers:
- **engine/** — C++ data node (28 .cpp / 5.8k LOC + 16 own headers; `third_party/` holds large vendored headers, e.g. a ~24.7k-line single-header lib — exclude from coverage). gRPC server. CMake build; test targets `test_{analysis,bitmap,wal,storage,scoring,vector,aggregation,index,segment,shard,edge_cases}`.
- **coordinator/** — Go (21 files / 5.4k LOC): REST API (chi), cluster state via **hashicorp/raft** + raft-boltdb, routing, replication, gRPC client to engines.
- `proto/engine.proto` — contract. `scripts/{build,run_cluster,stop_cluster,demo}.sh`. Docs: `DOCUMENTATION.md`, `docs/CODEBASE_REFERENCE.md`, `tests/TEST_DOCUMENTATION.md`, `examples/`.

## 2. Engine modules (`engine/src/`)
| Module | Responsibility |
|---|---|
| `analysis` | tokenisers, filters (lowercase, stop words, stemming?) |
| `index` | inverted index, postings |
| `bitmap` | compressed bitmaps for postings/filters (roaring-style) |
| `segment` | immutable segments; flush; merge |
| `storage` | on-disk formats, doc store |
| `wal` | write-ahead log / translog |
| `scoring` | BM25 / TF-IDF ranking |
| `vector` | vector index / kNN (ANN?) |
| `aggregation` | terms/metrics/histogram aggregations |
| `document` | document model / field mapping |
| `shard` | shard = WAL + in-memory buffer + segments; maintenance (`--maintenance-interval-sec`) |
| `rpc` | gRPC service implementation |

## 3. gRPC contract (`EngineService`)
`IndexDocument, GetDocument, DeleteDocument, BulkIndex, Search, CreateShard, Flush, Merge, ShardStatus, Health`.

## 4. Coordinator (`coordinator/internal/`)
`api` (REST), `cluster` (raft FSM: indices, shard table, node membership), `routing` (doc id → shard → node), `replication` (primary/replica writes), `engineclient`, `config`, `genproto`.
REST: `PUT /indices/{index}`, `PUT|GET|DELETE /indices/{index}/_doc/{id}`, `POST /indices/{index}/_bulk`, `POST /indices/{index}/_search`, `GET /_cluster/health`, `POST /_cluster/join`, `GET /health`.

## 5. Topology (`scripts/run_cluster.sh`)
engine-1 `:7001`, engine-2 `:7002` (`--port --data-dir --node-id --maintenance-interval-sec=10`); coordinator `-bootstrap -node-id=coord1 -http-addr=127.0.0.1:8090 -raft-addr=127.0.0.1:9010 -raft-dir=… -engine-nodes=localhost:7001,localhost:7002`. Flag `--keep-data` preserves data dirs. No Docker/compose — bare processes. For HA tests start 3 coordinators (join via `/_cluster/join`).

## 6. Existing tests
Go: `coordinator/internal/api` (2), `routing` (1), `config` (1). C++: 11 unit targets + `engine/tests/edge_cases_test.cpp`. No cluster-level, load, chaos or durability automation — the gap this plan fills.

## 7. Complexity & risk hot-spots
| # | Area | Risk |
|---|---|---|
| 1 | WAL + flush + segment lifecycle | ack-before-fsync; torn WAL tail; crash between segment write and WAL truncation (double apply or loss); crash mid-merge leaving both old+new or neither; manifest atomicity |
| 2 | Deletes/updates | tombstones across segments; update = delete+add atomicity; deleted docs resurfacing after merge; doc count & scoring stats (df, avgdl) including deleted docs |
| 3 | Replication | primary/replica divergence; write acked with only primary; replica catch-up after downtime; ordering of concurrent updates to same id (versioning / seqno) |
| 4 | Raft cluster state | coordinator leader loss; stale routing table on followers; join/leave; boltdb durability; write to follower forwarding |
| 5 | Scatter-gather search | partial shard failure (partial results vs error); global vs per-shard IDF (score comparability); top-k merge correctness & deterministic tie-break; pagination (from/size) deep paging; timeouts |
| 6 | Near-real-time semantics | refresh visibility: when does an indexed doc become searchable? GET (realtime from WAL/buffer) vs Search |
| 7 | C++ memory safety | parsers of on-disk formats & gRPC inputs: OOB, UAF, integer overflow (ASan/UBSan/TSan, libFuzzer mandatory) |
| 8 | Bitmap ops | container transitions (array↔bitmap↔run), AND/OR/NOT on boundaries (65535/65536), empty/full sets |
| 9 | Analysis | Unicode, case folding, very long tokens, empty text, analyser mismatch between index and query time |
| 10 | Vector search | dimension mismatch, NaN, recall vs brute force, deleted vectors, filter + kNN |
| 11 | Aggregations | per-shard top-N truncation error on terms agg; empty buckets; numeric overflow; float sum order |
| 12 | Routing | hash stability across versions; shard count change (reindex?); doc id edge cases |
| 13 | Bulk | partial failure reporting per item; huge bulk memory; ordering inside one bulk for same id |
| 14 | Concurrency in shard | concurrent index/search/flush/merge — reader snapshots, lock ordering |

## 8. Invariants
1. **Durability**: a write acked (HTTP 2xx) survives crash of any single engine and of the coordinator.
2. **No resurrection / no phantom**: after acked DELETE, the doc is never returned by GET or Search again (post-refresh), including after flush/merge/restart.
3. **Search = model**: for any query, hits == brute-force evaluation over the set of live docs (same analyser), ordered by score desc with deterministic tie-break; `total` exact (or documented lower bound).
4. **Replica convergence**: after quiescence, all copies of a shard hold identical live doc sets & versions.
5. **Last-write-wins by version**: concurrent updates to an id converge to one value on all copies.
6. **Cluster state**: ≤1 raft leader per term; committed index/shard metadata never lost.
7. Engine never crashes on any gRPC input or any on-disk byte corruption (fails with an error instead).

## 9. Build/run
```
./scripts/build.sh            # builds engine (cmake) + coordinator (go) into bin/
./scripts/run_cluster.sh      # 2 engines + 1 coordinator at 127.0.0.1:8090
./scripts/demo.sh ; ./scripts/stop_cluster.sh
cd engine && cmake -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
cd coordinator && go test ./...
```

## 10. Caveat
Index-creation body (shards/replicas/mappings), bulk format (NDJSON vs JSON array), query DSL, search response shape and refresh semantics were not read — confirm in `coordinator/internal/api`, `examples/`, `scripts/demo.sh` (authoritative request samples) and `DOCUMENTATION.md`.

# distributed-search-engine — Detailed Test Reference

Audience: LLM/QA agents. Nothing executed. **[src]** = read from `coordinator/` and `engine/`. Scripts (client + brute-force oracle, chaos model checker, corruption, sanitizer/fuzz builds): `03-TEST-SCRIPTS.md`.
Format: ID · Setup · Action · Expected · Priority · Tags.

---------------------------------------------------------------------------------------------------
## 1. System facts [src]
| Area | Fact |
|---|---|
| REST (coordinator, `-http-addr` e.g. :8090) | `PUT /indices/{index}`, `PUT/GET/DELETE /indices/{index}/_doc/{id}`, `POST /indices/{index}/_bulk`, `POST /indices/{index}/_search`, `GET /_cluster/health`, `POST /_cluster/join`, `GET /health` |
| Create index body | `{shards:int, replicas:int, mappings:[{field, type, vector_dim}]}` |
| Index doc response | `{doc_id, version}`; GET → `{doc_id, found, source}`; DELETE → `{doc_id, found}` |
| Bulk | request `{ops:[{type, doc_id, body}]}` → `{results:[{doc_id, success, error}]}` |
| Search request | `{query:string, fields:[string], filter:{term:[{field,value}], range:[{field, gte?, lte?}]}, vector:{field, embedding:[float32]}, vector_weight:float, aggregations:[{name, type (terms\|histogram\|stats…), field, size, interval}], size, from}` |
| Search response | `{hits:[{doc_id, score, source}], total_hits, aggregations:[{name, buckets:[{key, doc_count}], stats:{count,sum,min,max,avg}}], took_millis}` |
| Cluster | `ClusterStatus{nodes, is_leader, leader, node_id, shards:[{shard_id, index, primary, replicas[]}]}` |
| Errors | `missing index name`, `missing doc id`, `invalid JSON document body`, `failed to read request body` (400); `api: index not found`; `no engine nodes registered with the cluster`; `not the leader; current leader is …`; `this node is not the raft leader (leader is …)`; `dial engine node …`; config validation `NumShards`, `ReplicationFactor`, `HTTPAddr`, `RaftAddr`, `RaftDir`, `JoinAddr`, `EngineNodes`, `Bootstrap`, auto-generated `NodeID` |
| Replication | writes **synchronous to primary**, **asynchronous best-effort fire-and-forget to replicas** (failures only logged) → read-your-writes on primary, eventual (possibly divergent) on replicas; no repair mechanism stated |
| Raft | hashicorp/raft + raft-boltdb; create-index committed through raft log; non-leader returns leader hint |
| Engine flags | `--port`, `--data-dir`, `--node-id`, `--maintenance-interval-sec` |
| Engine WAL | record `[doc_id_len …][payload][CRC32 IEEE]`, **fsync per append** (`WriteAheadLog: fsync failed for …` on error); replay stops at CRC mismatch |
| Analyzer | tokenize → ASCII `tolower` per byte (**not Unicode casefolding**) → English stopword removal; tokenizer keeps `well-known`, `don't` as single tokens |
| BM25 | k1 = 1.2, b = 0.75, idf = ln(1 + (N − df + 0.5)/(df + 0.5)) (non-negative) |
| gRPC | `IndexDocument, GetDocument, DeleteDocument, BulkIndex, Search, CreateShard, Flush, Merge, ShardStatus, Health` |
| Topology script | engines :7001/:7002, coordinator :8090, raft :9010, `--keep-data` |

---------------------------------------------------------------------------------------------------
## 2. Environments & data
* **E-2E1C** (`run_cluster.sh`): 2 engines, 1 coordinator. **E-3E3C**: 3 engines, 3 coordinators (join). **E-SAN**: engines built with ASan/UBSan or TSan.
* Corpora: C-TINY (crafted for BM25/analyzer goldens), C-RAND-10k (seeded synthetic using a 300-word vocabulary + edge tokens: `well-known`, `don't`, `ÜNÏCODE`, `日本語`, stopwords), C-WIKI-1M (realistic), C-VEC (1M × 384-d normalised floats), C-NUM (numeric fields for range/aggregations incl. negatives, 1e15, NaN-free).
* Oracle: Python brute force implementing exactly §1 analyzer (ASCII lower + same stopword list from `analyzer.cpp`) and BM25; for multi-shard indices compare **sets** and per-shard-local scores (idf is per shard unless DFS).

---------------------------------------------------------------------------------------------------
## 3. Smoke
DSE-SMK-001 `GET /health`, `/_cluster/health` (nodes 2, leader true) · 002 gRPC Health each engine · 003 create index `{shards:2,replicas:1}` → index doc → GET found → search finds → delete → not found · 004 `scripts/demo.sh` · 005 stop + `run_cluster.sh --keep-data` → data persists.

---------------------------------------------------------------------------------------------------
## 4. Index management
| ID | Action | Expected | Pri |
|---|---|---|---|
| DSE-IDX-001 | create with shards 1/2/8, replicas 0/1 | shard table in cluster status; primaries/replicas on distinct nodes | P0 |
| DSE-IDX-002 | replicas ≥ number of engines (e.g. 2 with 2 engines) | rejected or placed on same node (bad) — document | P1 |
| DSE-IDX-003 | shards 0 / −1 / 10000; replicas −1 | 400 | P1 |
| DSE-IDX-004 | mappings: text, keyword, numeric, vector with `vector_dim` 0/−1/384/65536; unknown type; duplicate field | validated | P1 |
| DSE-IDX-005 | duplicate index name | 409 | P1 |
| DSE-IDX-006 | names: empty → `missing index name`; `..`, `/`, `_cluster`, `UPPER`, 300 chars, unicode | 400; no path escape on engines (`--data-dir`) | P0 |
| DSE-IDX-007 | create on follower coordinator (E-3E3C) | forwarded or error with leader hint (`not the leader; current leader is …`) — never silently lost | P0 |
| DSE-IDX-008 | create with no engines registered | `no engine nodes registered with the cluster` | P1 |
| DSE-IDX-009 | ops on missing index | 404 `api: index not found` | P1 |
| DSE-IDX-010 | delete index (if supported) | data removed on all engines | P2 |

## 5. Document CRUD
| ID | Action | Expected | Pri |
|---|---|---|---|
| DSE-DOC-001 | PUT doc id 1 | `{doc_id:"1", version:1}`; PUT again → version 2 | P0 |
| DSE-DOC-002 | GET existing / missing | `found:true, source` byte-equivalent JSON / `found:false` (status 404 or 200 — record) | P0 |
| DSE-DOC-003 | DELETE existing / missing | `found:true` / `found:false`; GET after → not found | P0 |
| DSE-DOC-004 | body invalid JSON / array / scalar / empty | 400 `invalid JSON document body` / defined | P1 |
| DSE-DOC-005 | doc id empty → `missing doc id`; ids with `/` (URL-encoded), `..`, unicode, 1 KB, whitespace | stored literally or 400; routing stable | P1 |
| DSE-DOC-006 | doc 10 MB, 10k fields, depth 1000 | bounded / 413 | P1 |
| DSE-DOC-007 | field type conflict with mapping (string into numeric, wrong vector dim) | 400 | P1 |
| DSE-DOC-008 | realtime GET right after PUT (primary) ×10k | 0 misses | P0 |
| DSE-DOC-009 | GET served by replica (if reads go to replicas) right after PUT | may be stale (async replication) → measure staleness | P1 |
| DSE-DOC-010 | version monotonic per id across updates; concurrent updates | final version = number of acked updates on primary | P1 |

## 6. Bulk
DSE-BLK-001 `{ops:[{type:"index",doc_id,body}...]}` 1/1000/100k → `results` per op, success counts · 002 mixed index/delete ops; same id repeated (final = last op) · 003 one invalid op in the middle (bad JSON body, unknown type) → that item `success:false, error`, others applied · 004 `ops` empty/missing/not array → 400 · 005 ops spanning all shards; replica failure during bulk → primary result still success (replica divergence) · 006 1 GB bulk → bounded memory / 413.

## 7. Search
| ID | Action | Expected | Pri |
|---|---|---|---|
| DSE-SRC-001 | `query:"alpha"` over C-RAND-10k | hit set == oracle; `total_hits` exact; no duplicate doc_ids | P0 |
| DSE-SRC-002 | multi-term `"w1 w2"` | OR semantics (any term) vs AND — record and assert consistently with oracle | P0 |
| DSE-SRC-003 | `fields:["title"]` restricts | only matches in title | P1 |
| DSE-SRC-004 | analyzer: `"ALPHA"` == `"alpha"`; `"ÜNÏCODE"` vs `"ünïcode"` (**ASCII-only lowercasing → no match**, document); `"the"` (stopword) → zero hits; `"well-known"` matches token `well-known`, not `well`; `"don't"` | P1 |
| DSE-SRC-005 | BM25 order on C-TINY single shard: tf, length norm, idf; tie-break deterministic | exact golden order & scores (4 dp) | P0 |
| DSE-SRC-006 | extreme: term in every doc (idf ≈ ln(1+0.5/(N+0.5)) small but > 0), N=1, empty field, avgdl 0 | finite scores, no NaN/Inf | P0 |
| DSE-SRC-007 | multi-shard score comparability: same doc set in 1-shard vs 4-shard index | order may differ (local idf) → quantify; document | P2 |
| DSE-SRC-008 | filter term exact match (keyword) + query | intersection | P0 |
| DSE-SRC-009 | filter range `gte`/`lte` inclusive; only gte; only lte; gte>lte; on non-numeric field | correct / empty / 400 | P0 |
| DSE-SRC-010 | filter only (empty query) | match-all filtered? or zero — document | P1 |
| DSE-SRC-011 | vector `{field, embedding}` k via size | top-k == brute-force cosine/dot (exact) or recall ≥ 0.95 | P0 |
| DSE-SRC-012 | hybrid `query` + `vector` with `vector_weight` 0 / 0.5 / 1 / −1 / 2 | 0 = lexical only; 1 = vector only; invalid rejected | P1 |
| DSE-SRC-013 | vector dim mismatch, NaN, empty, zero-norm | 400 | P1 |
| DSE-SRC-014 | `size` 0/1/10/10000/−1/1e9; `from` 0/−1/total/total+1 | clamps/400; deep paging bounded | P1 |
| DSE-SRC-015 | pagination union == full result, stable for ties | P0 |
| DSE-SRC-016 | aggregations `terms` size N: counts == oracle (multi-shard top-N truncation error measured) | P1 |
| DSE-SRC-017 | `histogram` interval 10, 0, negative, 1e-9 | buckets correct / 400 | P1 |
| DSE-SRC-018 | `stats` count/sum/min/max/avg; empty result set (avg undefined) | no NaN in JSON | P1 |
| DSE-SRC-019 | aggregation on missing field / text field | empty / 400 | P2 |
| DSE-SRC-020 | `took_millis` present & sane | P3 |
| DSE-SRC-021 | partial shard failure (one engine down, replicas 0) | error or partial result with flag — never silently fewer hits without indication | P0 |
| DSE-SRC-022 | 10k-term query string, 1 MB query | bounded | P1 |

## 8. Replication & cluster
| ID | Action | Expected | Pri |
|---|---|---|---|
| DSE-REP-001 | index 10k docs, replicas 1, then compare primary vs replica doc sets (ShardStatus / per-engine search) | equal after quiescence (async) | P0 |
| DSE-REP-002 | stop replica engine, index 1k, restart | **replica missing 1k docs permanently** (no catch-up) → P1 finding; expected anti-entropy/repair | P1 |
| DSE-REP-003 | primary down | writes fail (no promotion?) — document failover behaviour | P0 |
| DSE-REP-004 | delete while replica down → resurrect on replica-served reads | finding | P1 |
| DSE-REP-005 | concurrent updates same id → replica order may differ (async, no seqno) → divergence | measure (I5) | P1 |
| DSE-CLU-001 | `/_cluster/join` new coordinator/engine | membership on all coordinators | P1 |
| DSE-CLU-002 | join garbage / duplicate node id / unreachable / self | rejected; state intact | P1 |
| DSE-CLU-003 | `/_cluster/join` unauthenticated from any host | cluster hijack risk → document / restrict | P1 |
| DSE-CLU-004 | 3 coordinators: kill leader | new leader; index metadata intact; clients redirected | P0 |
| DSE-CLU-005 | coordinator restart: boltdb raft log & snapshots restore FSM | P0 |
| DSE-CLU-006 | config validation errors (each field) | clear startup failure | P2 |

## 9. Engine internals (C++ unit/integration, existing 11 targets + additions)
DSE-ENG-001 WAL: fsync per append (strace shows fsync), CRC detects flipped byte, torn tail at every offset → replay stops cleanly · 002 segment flush/merge model test (random add/delete across segments; merge; deleted never present; failpoints at each merge stage) · 003 bitmap container boundaries (ids 0, 65535, 65536, 2^31−1, 2^32−1; 4095/4096/4097 cardinality) vs std::set · 004 analyzer goldens incl. long tokens (1 MB), empty, punctuation-only · 005 scoring extremes · 006 vector index recall/deletes · 007 aggregation numeric edge (int64 overflow in sum, float accumulation order) · 008 shard concurrent index/search/flush/merge under TSan · 009 gRPC handlers with random protobufs (structure-aware fuzz) · 010 maintenance interval 1 s under load.

## 10. E2E
DSE-E2E-001 C-WIKI-1M bulk → 500-query suite vs oracle → aggregations → restart → identical · 002 30-min upsert/delete/search churn → full model comparison · 003 vector + hybrid workload · 004 rolling restart engines then coordinators under load → zero failed (retried) client ops; final == model on primaries · 005 scale-out add engine (new indices use it).

## 11. Concurrency / consistency
DSE-RACE-001 32 writers + 16 searchers + Flush/Merge loop 10 min → final == model; no duplicate hits · 002 50 concurrent updates one id → primary final = one of them; replica may diverge (I5) · 003 index vs delete same id · 004 search during merge/flush · 005 concurrent index creation same name ×20 → one · 006 concurrent joins · 007 bulk overlapping ids · 008 maintenance vs writes (TSan) · 009 sanitizers clean · DSE-CON-001 RYW on primary ×10k · 002 refresh/visibility latency p99 · 003 primary vs replica diff · 004 total_hits == model · 005 via different coordinators same results · 006 score comparability documented · 007 linearizability history check for single-doc ops on primary.

## 12. Durability
DSE-DUR-001 ack N → kill -9 engine → all N (fsync per record) · 002 kill during bulk storm ×50 → acked present, no partial docs · 003 WAL truncate/bit-flip · 004 crash at each flush/merge stage · 005 corrupt/truncate segment or manifest → shard red, no segfault · 006 deletes durable through merge+restart · 007 coordinator kill -9 → raft state intact · 008 disk full on WAL/flush/merge/raft · 009 cold start in any order.

## 13. Load / stress / spike / soak / scalability
DSE-LOAD-001 bulk 1M docs, 8 clients → docs/s baseline (fsync per record bounds throughput — measure) · 002 search 500 qps mixed → p50 < 20 ms, p99 < 200 ms (calibrate) · 003 80/20 search/index 30 min · 004 kNN 100 qps 1M×384 · DSE-STR-001 ramp qps · 002 indexing > merge rate → bounded segments/memory · 003 deep paging/huge bool/high-cardinality terms agg · 004 10k indices · DSE-SPK-001 · DSE-SOAK-001 24 h (C++ RSS, FDs, disk plateau, raft log compaction) · DSE-SCAL-001..003 engines, corpus size, shard counts.

## 14. Chaos
DSE-CHA-001 kill engine (replicas 1) → reads/writes behaviour per REP-003 · 002 replica catch-up (REP-002) · 003 kill raft leader (E-3E3C) · 004 single coordinator restart · 005 partitions coordinator↔engine, engine↔engine, minority coordinator · 006 slow engine (SIGSTOP / 500 ms) → search timeouts, partial flag · 007 gRPC resets · 008 disk full / RO on one engine · 009 crash during flush/merge loop · 010 clock jump · 011 OOM-kill during agg · 012 rolling restart · 013 engine replaced with empty data dir → shard must not report green-and-empty.

## 15. Known findings from source
1. Replicas receive writes asynchronously, best-effort, with no catch-up/repair → permanent divergence after replica downtime (P1). 2. No failover/promotion evident when primary is down. 3. Analyzer lowercases ASCII only. 4. `/_cluster/join` unauthenticated. 5. Per-record fsync bounds indexing throughput (design trade-off to benchmark).

## 16. Exit criteria
Model equivalence on primaries for 20 seeds incl. crash injection; replica divergence documented/fixed; sanitizers + fuzzers clean; DUR-003/004 pass at every offset/stage; perf within 10 % of baseline.

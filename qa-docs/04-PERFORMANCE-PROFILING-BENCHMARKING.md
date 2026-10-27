# distributed-search-engine (C++ engine + Go coordinator) — Performance, Profiling & Benchmarking Reference (04)

Audience: LLM agents / engineers. Companion to 01/02/03. **[src]** = read from source. Targets are hypotheses to calibrate. Nothing executed.

## 0. Limits
Covers indexing throughput, search latency/QPS, relevance-quality-at-speed (BM25, hybrid vector), segment/merge behaviour, replication cost and cluster scaling for the listed corpora and query sets. Passing does not prove the absence of performance defects for other corpora, query distributions, or hardware.

## 1. Performance model [src]

- **Coordinator (Go)** `-http-addr=127.0.0.1:8090`, Raft `-raft-addr=127.0.0.1:9010` (hashicorp/raft) for cluster metadata. HTTP API: `PUT /indices/{index}`, `POST /indices/{index}/_bulk` (`{ops:[{type,doc_id,body}]}`), `POST /indices/{index}/_search` (`{query, fields, filter{term,range}, vector, vector_weight, aggregations, size, from}` → `{hits[{doc_id,score,source}], total_hits}`), `DELETE /indices/{index}/_doc/{id}`, `GET /_cluster/health`, `POST /_cluster/join`, `GET /health`. Scatter-gather search to engine shards over gRPC (`internal/engineclient`, `routing`). Replication: primary write synchronous, replicas **async best-effort with `asyncReplicaTimeout = 5s`** [src replication.go:27], no catch-up.
- **Engine (C++ gRPC)** `dse_engine --port --data-dir --node-id --maintenance-interval-sec=10` (engine-1 :7001, engine-2 :7002). `grpc::ServerBuilder` default sync server (thread pool managed by gRPC), `SetMaxReceiveMessageSize(64 MiB)` [src main.cpp:60]. Background LSM maintenance thread every N s [src main.cpp:43-51].
- **Per-shard exclusive `std::mutex mu_` taken by `Index`, `Get`, `Delete`, `Bulk`, `Search`, `Flush`, `Merge`, `Status` [src shard.cpp:254-644]** ⇒ on one shard, searches serialize with each other *and* with writes and merges. A long merge or a big bulk blocks all searches on that shard. Concurrency comes only from multiple shards. Engine-level `shards_mu_` also guards the shard map.
- Index structures: inverted index (`index/inverted_index.cpp`), Roaring bitmaps (`bitmap/`), segments + `segment_merger.cpp`, WAL **fsync per record** [02 facts], HNSW (`vector/hnsw.h`: `M=16`, `ef_construction=200`, `ef_search=50` defaults), BM25 k1=1.2 b=0.75, analyzer ASCII-lowercase + stopwords, aggregations (`aggregation/`), scoring (`scoring/`).
- Build: `CMAKE_BUILD_TYPE` defaults to Release with **`-O2`** [src CMakeLists.txt:10] (no `-march=native`, no LTO). Executables: `dse_engine` + test binaries.
- Metrics: **no `/metrics`/pprof found** [src grep] ⇒ client-side + OS measurement; add Go pprof/promhttp to coordinator in perf build.
- Scripts: `scripts/{build.sh, run_cluster.sh (2 engines + 1 coordinator), stop_cluster.sh, demo.sh}`.

## 2. Environment rules
Record CPU (AVX2/AVX-512 — relevant to vector distance), compiler + version + flags, gRPC version, disk (fsync latency via fio), Go version, shard count per index, replicas, maintenance interval. Corpus and query set fixed with checksums (§3.2). Warm OS page cache explicitly for "hot" runs; drop caches for "cold" runs. Generator on separate cores/host. 3 reps, CV < 5%.

## 3. Data-retrieval contract
`perf-results/dse/<YYYYMMDD-HHMM>-<sha>/<scenario>/`: `meta.json`, `index_load.csv` (batch, docs/s, bytes/s per interval), `search.csv` (query_id, t_send, latency_ms, total_hits, top10 ids), `relevance.json` (nDCG@10, recall@k vs qrels / brute-force), `k6-summary.json`, `ghz-*.json` (direct engine), `engine/{perf.data,oncpu.svg,offcpu.svg,perfstat.txt,heaptrack.zst}`, `coordinator/pprof/*`, `disk/{du.csv,fsync_lat.txt,segments.csv}`, `summary.json`.
```json
{"project":"dse","scenario":"PERF-SRCH-03","git":"…","corpus":"msmarco-1M","shards":2,"replicas":1,
 "qps":850,"conc":32,"lat_ms":{"p50":12,"p99":61,"p999":140},"index_docs_s":0,"merge_during_run":true,
 "quality":{"ndcg10":0.34,"recall_vec_at10":0.93},"engine":{"cpu_cores":7.1,"rss_mb":5200,"segments":14},"pass":true}
```
Segment/disk sampling every 10 s: `du -sb $DATA_DIR/*` and count segment files (`find $DATA_DIR -name '*.seg*' | wc -l` — VERIFY extension) → `disk/segments.csv`.

### 3.1 Corpora & query sets (store checksums)
| Name | Docs | Notes |
|---|---|---|
| `wiki-abstracts-1M` | 1M | English text; title/body fields |
| `msmarco-passages` (1M/8.8M) | | has qrels ⇒ nDCG/MRR |
| `logs-synthetic-10M` | 10M | short docs, numeric `range` filters, term filters (status, service) |
| `vectors-sift1M` (128d) / `glove-100` | 1M | ground-truth kNN for recall |
| `hybrid-1M` | 1M | text + 384d embeddings for `vector_weight` blends |
Query sets: 10k queries each: single-term (common/rare), multi-term OR, filter-only, range, aggregations (terms agg on 1k-cardinality field), deep pagination (`from` 10k), vector-only (k=10/100), hybrid (`vector_weight` 0.3/0.7).

## 4. Tool matrix
| Need | Tool | Command |
|---|---|---|
| HTTP search/bulk load | k6, vegeta, `wrk2` with Lua body | §8 |
| Direct engine gRPC | `ghz` | `ghz --insecure --proto proto/engine.proto --call dse.Engine/Search -D q.json -c 32 -z 60s --format json -o ghz.json 127.0.0.1:7001` (VERIFY package/service names in `proto/`) |
| Relevance | `trec_eval`, `ir-measures`, `ranx` (Python) | nDCG@10, MRR@10, recall@k |
| Vector recall | brute-force numpy/FAISS flat ground truth | recall@k vs ef_search |
| C++ micro benches | Google Benchmark (add) | `--benchmark_format=json --benchmark_out=…` |
| C++ CPU | `perf record -g --call-graph dwarf`, `perf stat`, Intel VTune / AMD uProf, `hotspot` GUI | |
| C++ lock contention | `perf lock`, `offcputime-bpfcc`, `bpftrace` on `pthread_mutex_lock` (uprobe) | per-shard mutex wait |
| C++ memory | `heaptrack`, `valgrind --tool=massif`, `jemalloc` prof (`LD_PRELOAD` + `MALLOC_CONF=prof:true`) | |
| Cache/branch | `perf stat -e cache-misses,LLC-load-misses,branch-misses`, `valgrind --tool=cachegrind` | posting list traversal |
| Sanitizers (before trusting numbers) | `-fsanitize=address,undefined`, `-fsanitize=thread` builds | |
| Go coordinator | pprof (perf build), `go tool trace`, benchstat | |
| Disk | `fio --fsync=1`, bpftrace fsync hist, `iostat -x` | |
| Comparisons | OpenSearch/Elasticsearch or Tantivy/Lucene on same corpus (reference only) | |

## 5. Micro-benchmarks (Google Benchmark; add `engine/bench/*.cc`, CMake target `dse_bench`)
| ID | Target | Params | Record |
|---|---|---|---|
| MB-01 | analyzer/tokenizer | doc 200 B/5 KB; ASCII vs UTF-8 | MB/s |
| MB-02 | inverted index add doc | fields 1/5 | docs/s |
| MB-03 | posting-list intersection (Roaring AND/OR) | lists 1k/1M; selectivity | ns/op |
| MB-04 | BM25 scoring top-k (heap) | hits 1k/1M, k 10/100 | ns/op |
| MB-05 | range filter on numeric field | 10M docs | ns/op |
| MB-06 | terms aggregation | cardinality 10/1k/100k | ns/op, memory |
| MB-07 | HNSW insert | N 100k/1M, dim 128/384, M 16/32, ef_c 200 | inserts/s, memory/vector |
| MB-08 | HNSW search | ef_search 10/50/200, k 10 | QPS vs recall curve |
| MB-09 | distance kernel L2/cosine | dim 128/384/768 | GFLOP/s; check vectorization (`-O2` without `-march`) |
| MB-10 | segment write/flush | 100k docs | MB/s |
| MB-11 | segment merge | 10 × 100k | MB/s, peak memory |
| MB-12 | WAL append (fsync per record) | 1 KB | records/s (≈ fsync rate) |
| MB-13 | JSON parse of `_source` / bulk | 1 KB docs | MB/s |
| MB-14 (Go) | coordinator merge of shard results (top-k merge, agg merge) | shards 2/16, k 10/1000 | ns/op |
```cpp
#include <benchmark/benchmark.h>
static void BM_HnswSearch(benchmark::State& s){ auto idx=BuildHnsw(1'000'000,128,16,200); auto qs=LoadQueries(1000);   // helpers; VERIFY hnsw.h API
  size_t i=0; for(auto _: s){ benchmark::DoNotOptimize(idx.Search(qs[i++%qs.size()], 10, s.range(0))); } s.SetItemsProcessed(s.iterations()); }
BENCHMARK(BM_HnswSearch)->Arg(10)->Arg(50)->Arg(200)->Unit(benchmark::kMicrosecond);
BENCHMARK_MAIN();
```
Compiler-flag study: rebuild with `-O3 -march=native -flto` and compare MB-03/04/08/09 (documents the cost of the `-O2` default).

## 6. Macro scenarios
### 6.1 Indexing
| ID | Workload | Measure | Target (calibrate) |
|---|---|---|---|
| PERF-IDX-01 | `_bulk` batch 1/100/1000/5000 docs, 1 client | docs/s, MB/s, p99 per bulk | gains flatten when fsync amortized? (WAL fsync per record ⇒ no) |
| PERF-IDX-02 | 1/4/16 parallel bulk clients, shards 1/2/4 | docs/s scaling (per-shard mutex ⇒ scales with shards only) | |
| PERF-IDX-03 | Bulk near 64 MiB gRPC limit [src] | success, memory spike | |
| PERF-IDX-04 | With vectors (384d) vs text only | docs/s; HNSW insert dominates? | |
| PERF-IDX-05 | Replicas 0/1 | write latency (primary sync) unchanged; replica lag/timeout drops (5 s) | |
| PERF-IDX-06 | Sustained ingest 4 h | segment count, merge frequency, disk growth, write stalls during merge | |

### 6.2 Search
| ID | Workload | Measure |
|---|---|---|
| PERF-SRCH-01 | single-term queries, hot cache, conc 1 | p50 floor (coordinator + gRPC + shard) |
| PERF-SRCH-02 | conc 1/4/16/64 per query class | QPS, p99; QPS saturates at ≈ shards × (1/service_time) due to per-shard mutex |
| PERF-SRCH-03 | search during max bulk ingest | p99 inflation (reader/writer serialization) |
| PERF-SRCH-04 | search during merge | p99/max stall ≈ merge duration ⇒ finding if seconds |
| PERF-SRCH-05 | filters/range only | |
| PERF-SRCH-06 | aggregations high cardinality | memory, p99 |
| PERF-SRCH-07 | deep pagination `from` 0/1k/10k | cost grows with from+size per shard × shards |
| PERF-SRCH-08 | vector k=10, ef 50 (default) vs 200 | QPS vs recall table |
| PERF-SRCH-09 | hybrid weights 0.3/0.7 | p99, nDCG vs pure BM25 |
| PERF-SRCH-10 | cold cache (drop caches) | first-query latency, IO |
| PERF-SRCH-11 | 1 shard slow (cgroup CPU throttle / tc delay on 7002) | scatter-gather tail = slowest shard; timeouts/partial results behaviour |

### 6.3 Cluster / resilience
| ID | Procedure | Measure |
|---|---|---|
| PERF-CL-01 | engines 1/2/4 (shards spread) | USL for search QPS and indexing docs/s |
| PERF-CL-02 | kill engine during load | error rate window, recovery; replica divergence (no catch-up) |
| PERF-CL-03 | coordinator Raft leader change (3 coordinators) | metadata ops pause; search unaffected? |
| PERF-REC-01 | restart engine with 10M-doc WAL/segments | startup time: LoadExistingSegments + ReplayWAL + `RebuildVectorIndexFromCurrentState` [src] (HNSW rebuild can dominate: N × insert cost) |
| PERF-SOAK-01 | mixed 90% search/10% write 12 h | RSS flat, segment count bounded, p99 stable |

## 7. Profiling playbooks
### 7.1 Build for profiling
`cmake -S engine -B build-prof -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-O2 -g -fno-omit-frame-pointer"` (keep -O2 to profile what ships; optional second build with -O3 -march=native).
### 7.2 CPU & locks
```bash
PID=$(pgrep -f 'dse_engine.*7001')
perf record -F 999 -g --call-graph fp -p $PID -- sleep 30; perf script | stackcollapse-perf.pl | flamegraph.pl > $OUT/engine/oncpu.svg
perf report --sort symbol --stdio --no-children | head -60 > $OUT/engine/top.txt
perf stat -e cycles,instructions,cache-misses,LLC-load-misses,branch-misses,context-switches -p $PID -- sleep 30 2> $OUT/engine/perfstat.txt
sudo offcputime-bpfcc -df -p $PID 30 | flamegraph.pl --color=io > $OUT/engine/offcpu.svg
# mutex wait time distribution (glibc)
sudo bpftrace -e 'uprobe:/lib/x86_64-linux-gnu/libc.so.6:pthread_mutex_lock /pid==PID/ {@s[tid]=nsecs}
  uretprobe:/lib/x86_64-linux-gnu/libc.so.6:pthread_mutex_lock /pid==PID && @s[tid]/ {@wait_us=hist((nsecs-@s[tid])/1000); delete(@s[tid])}' > $OUT/engine/mutex_wait.txt
```
Expected: off-CPU dominated by `pthread_mutex_lock` (Shard::mu_) under concurrent search, and by `fsync` during indexing; on-CPU by posting traversal, BM25 scoring, HNSW distance (scalar if not vectorized), JSON parsing.
### 7.3 Memory
```bash
heaptrack ./build-prof/dse_engine --port 7001 --data-dir /tmp/e1 --node-id e1   # run load, stop; heaptrack_print … > $OUT/engine/heaptrack.txt
valgrind --tool=massif --pages-as-heap=yes ./build-prof/dse_engine …              # slower, peak breakdown
```
Track bytes/doc (text index), bytes/vector (HNSW: dim×4 + M×2×4 per level-0 link approx).
### 7.4 Coordinator (Go)
Perf build: pprof on 127.0.0.1:6060; capture cpu/heap/goroutine during PERF-SRCH-02. Expected: JSON encode/decode of hits (`source` blobs), gRPC client marshalling, result merge.
### 7.5 Expected signatures
| Symptom | Evidence | Cause |
|---|---|---|
| QPS flat as concurrency ↑, CPU < cores | mutex_wait hist, offcpu | per-shard exclusive mutex [src] |
| Search p99 = seconds during merge/flush | timeline vs maintenance interval | Merge holds `mu_` [src] |
| Indexing docs/s ≈ fsync/s | fsync hist | WAL fsync per record |
| Vector search slower than expected | perf annotate distance loop scalar | `-O2` without `-march` |
| Startup minutes on large index | timing logs | HNSW rebuild on load |
| Replica results differ | compare queries per replica | async best-effort replication |

## 8. Scripts
### 8.1 Corpus loader (bulk, measures docs/s)
```python
# python3 load.py http://127.0.0.1:8090 myidx corpus.jsonl 1000 out.csv
import sys, json, time, requests
B,IDX,F,BATCH,OUT=sys.argv[1],sys.argv[2],sys.argv[3],int(sys.argv[4]),sys.argv[5]
requests.put(f"{B}/indices/{IDX}", json={})                       # VERIFY create-index body (mappings)
ops=[]; n=0; t0=time.time(); w=open(OUT,'w'); w.write("t,docs,docs_s,bulk_ms\n")
for line in open(F):
    d=json.loads(line); ops.append({"type":"index","doc_id":str(d.get("id",n)),"body":d}); n+=1
    if len(ops)==BATCH:
        s=time.time(); r=requests.post(f"{B}/indices/{IDX}/_bulk", json={"ops":ops}); r.raise_for_status(); ms=(time.time()-s)*1000
        w.write(f"{time.time():.3f},{n},{n/(time.time()-t0):.1f},{ms:.1f}\n"); ops=[]
print(f"docs={n} docs_s={n/(time.time()-t0):.1f}")
```
### 8.2 Search runner with relevance capture (open loop)
```python
# python3 search.py http://127.0.0.1:8090 myidx queries.jsonl 200 60 out.csv   (rate qps, duration s)
import sys, json, time, requests, threading, queue
B,IDX,Q,RATE,DUR,OUT=sys.argv[1],sys.argv[2],sys.argv[3],float(sys.argv[4]),int(sys.argv[5]),sys.argv[6]
qs=[json.loads(l) for l in open(Q)]; res=queue.Queue(); s=requests.Session()
def fire(i,q,intended):
    r=s.post(f"{B}/indices/{IDX}/_search", json={"query":q["text"],"size":10, **q.get("extra",{})})
    lat=(time.time()-intended)*1000; j=r.json() if r.ok else {}
    res.put((q["id"],intended,lat,r.status_code,j.get("total_hits"),[h["doc_id"] for h in j.get("hits",[])]))
t0=time.time(); i=0
while time.time()-t0<DUR:
    intended=t0+i/RATE; time.sleep(max(0,intended-time.time()))
    threading.Thread(target=fire,args=(i,qs[i%len(qs)],intended),daemon=True).start(); i+=1
time.sleep(5)
with open(OUT,'w') as w:
    while not res.empty(): qid,t,lat,st,tot,ids=res.get(); w.write(f"{qid},{t:.3f},{lat:.2f},{st},{tot},{'|'.join(ids)}\n")
```
Percentiles: `python -c "import pandas as p;d=p.read_csv('out.csv',header=None);print(d[2].quantile([.5,.9,.99,.999]))"`; relevance: convert to TREC run file and `trec_eval -m ndcg_cut.10 qrels run`.
### 8.3 Merge-stall detector
Run PERF-SRCH-02 at fixed QPS for 30 min with `--maintenance-interval-sec=10`; plot latency vs time; mark maintenance ticks (engine logs). Report max latency within ±2 s of each tick vs elsewhere.
### 8.4 Vector recall sweep
For ef ∈ {10,20,50,100,200}: query engine (`vector` field, k=10) and compare to FAISS `IndexFlatL2` ground truth → `recall@10`, QPS → `vector_tradeoff.csv`. (ef_search exposure via API — VERIFY; otherwise use MB-08.)

## 9. Scalability
USL over shards (not threads — per-shard mutex). Search: latency ≈ max over shards ⇒ more shards reduce per-shard work but raise tail via fan-out (model with order statistics). Indexing: docs/s ≈ shards × fsync_rate × docs_per_fsync.

## 10. Regression gates
Google Benchmark MB-*: +5% time ⇒ fail (use `compare.py` from benchmark tools); PERF-SRCH-02 class mix at fixed QPS: p99 ≤ 1.2×; nDCG@10 and recall@10 must not drop > 0.005; PERF-IDX-01 docs/s ≥ 0.9×.

## 11. Report template
```
Run <date> sha <sha> | CPU/ISA | compiler/flags | disk fsync p99 | shards/replicas | corpus/query-set checksums
Index: | batch | clients | docs/s | MB/s | bulk p99 | segments end |
Search: | class | conc/QPS | p50 | p99 | p99.9 | nDCG@10 | recall@10 | during-merge max |
Engine on/off-CPU tops; mutex wait histogram; memory/doc & /vector; findings
```

## 12. Risks from source
| # | Risk | Scenario |
|---|---|---|
| R1 | Single exclusive mutex per shard for search, writes, flush, merge | PERF-SRCH-02/03/04 |
| R2 | WAL fsync per record caps indexing | PERF-IDX-01 |
| R3 | Async best-effort replication (5 s timeout, no catch-up) ⇒ divergent replicas | PERF-IDX-05, CL-02 |
| R4 | HNSW rebuilt on startup | PERF-REC-01 |
| R5 | `-O2` default, no ISA flags ⇒ slow vector math | MB-09 |
| R6 | 64 MiB gRPC messages ⇒ memory spikes with huge bulks | PERF-IDX-03 |
| R7 | No metrics endpoints | §3 |

# dse — Distributed Search Engine: Complete Architectural Documentation

This document is the full blueprint for `dse`, a distributed search engine
built from scratch using a hybrid **Go (cluster/routing) + C++
(storage/execution)** architecture, in the spirit of Elasticsearch. It covers
every module's design, the exact request-execution flows, the on-disk and
on-wire formats, and — explicitly — every simplification made and why, so
nothing here is oversold.

Every piece of functionality described below is real, working, tested code.
Nothing is a stub. The whole system was validated with:
- 10 independent C++ test binaries (`ctest`), 100% passing
- 13 Go test functions, 100% passing
- A live, multi-process, scripted end-to-end demo (`scripts/demo.sh`) that
  creates a sharded+replicated index, indexes/updates/deletes documents,
  runs BM25 keyword search, term/range filters, three kinds of distributed
  aggregations, hybrid vector+keyword search, and — critically — a real
  `kill -9` crash of a data node followed by a verified full recovery.

---

## Table of contents

1. [Hybrid architecture overview](#1-hybrid-architecture-overview)
2. [Repository layout](#2-repository-layout)
3. [The gRPC contract (`proto/engine.proto`)](#3-the-grpc-contract-protoengineproto)
4. [C++ engine — module by module](#4-c-engine--module-by-module)
5. [Go coordinator — module by module](#5-go-coordinator--module-by-module)
6. [Execution flow: indexing a document](#6-execution-flow-indexing-a-document)
7. [Execution flow: scatter-gather search](#7-execution-flow-scatter-gather-search)
8. [Execution flow: distributed aggregations](#8-execution-flow-distributed-aggregations)
9. [Execution flow: hybrid vector + keyword search](#9-execution-flow-hybrid-vector--keyword-search)
10. [Execution flow: LSM flush, merge, and background maintenance](#10-execution-flow-lsm-flush-merge-and-background-maintenance)
11. [Execution flow: crash recovery](#11-execution-flow-crash-recovery)
12. [Cluster membership: Raft bootstrap and join](#12-cluster-membership-raft-bootstrap-and-join)
13. [On-disk formats](#13-on-disk-formats)
14. [REST API reference](#14-rest-api-reference)
15. [Consistency model](#15-consistency-model)
16. [Known simplifications and honest gaps](#16-known-simplifications-and-honest-gaps)
17. [Testing strategy](#17-testing-strategy)
18. [Extending the system](#18-extending-the-system)

---

## 1. Hybrid architecture overview

```
                          ┌─────────────────────────────────────────┐
                          │         Go Coordinator (any count)        │
   client ── HTTP/JSON ──▶│  REST API │ Raft cluster metadata │       │
                          │  scatter-gather routing │ replication fan-out│
                          └──────────────┬──────────────┬─────────────┘
                                         │ gRPC          │ gRPC
                          ┌──────────────▼───┐      ┌────▼──────────────┐
                          │  C++ Engine node  │      │  C++ Engine node   │
                          │ (data node)       │      │ (data node)        │
                          │  shard A (primary)│◀────▶│  shard A (replica) │
                          │  shard B (replica)│      │  shard B (primary) │
                          └───────────────────┘      └────────────────────┘
```

- **API & routing layer (Go, `coordinator/`)** — the single thing a client
  talks to. Exposes a JSON REST API, resolves which shard(s) a request
  targets, and for search specifically performs **scatter-gather**: fan a
  query out to every shard's primary in parallel, wait for all responses,
  and merge them into one globally-ranked, globally-paginated,
  globally-aggregated result.
- **Cluster management (Go, `coordinator/internal/cluster`)** — every
  coordinator node runs a [Raft](https://raft.github.io/) consensus instance
  (`github.com/hashicorp/raft`) replicating one small piece of state: which
  indices exist, how many shards each has, and which engine node holds the
  primary/replica copy of each shard. A new leader is elected automatically
  if the current leader crashes; every coordinator node has an identical,
  consistent view of shard placement.
- **Core execution engine (C++, `engine/`)** — one process per data node,
  exposing a gRPC service (`EngineService`, defined in `proto/engine.proto`).
  Owns zero or more **shards**; each shard does its own text analysis,
  inverted-index lookups, BM25 scoring, vector search, and local
  aggregation.
- **Storage layer (C++, `engine/src/{wal,storage,segment,bitmap}`)** — raw
  disk I/O: a write-ahead log for durability, memory-mapped immutable
  segment files for fast reads, and Roaring Bitmaps for compressed posting
  lists with fast set-algebra (AND/OR/AND-NOT).

Why this split? Go's goroutines and standard library make the
networking/coordination/consensus layer pleasant and safe to write; C++
gives full control over memory layout, `mmap`, and CPU-bound hot loops
(tokenization, postings intersection, BM25 scoring, HNSW graph traversal)
where allocation and cache behavior matter. This mirrors real systems in the
space (e.g., Vespa's C++ content layer + a higher-level orchestration
plane; Rockset's separation of a Go/Java control plane from a C++ execution
engine).

---

## 2. Repository layout

```
distributed-search-engine/
├── proto/engine.proto              the ONE contract between Go and C++
├── coordinator/                    Go module (module path: dse/coordinator)
│   ├── cmd/coordinator/main.go     binary entry point / wiring
│   └── internal/
│       ├── api/                    REST handlers + JSON types + Coordinator interface
│       ├── cluster/                Raft node, FSM, replicated shard-placement state
│       ├── config/                 CLI flag parsing
│       ├── engineclient/           gRPC client connection pool to engine nodes
│       ├── genproto/               generated protobuf/gRPC Go code (checked in)
│       ├── replication/            primary-then-async-replica write fan-out
│       └── routing/                THE scatter-gather coordinator (implements api.Coordinator)
├── engine/                          C++ project (CMake)
│   ├── CMakeLists.txt
│   ├── genproto/                    generated protobuf/gRPC C++ code (checked in)
│   ├── third_party/json.hpp         vendored nlohmann/json (single header, MIT license)
│   └── src/
│       ├── analysis/                Tokenizer, Analyzer (lowercase + stopwords)
│       ├── bitmap/                  RoaringBitmap (array/bitset containers, AND/OR/ANDNOT)
│       ├── wal/                     WriteAheadLog (CRC32-checksummed, fsync'd, replayable)
│       ├── storage/                 MMapFile (RAII mmap/munmap/msync wrapper)
│       ├── scoring/                 BM25Scorer
│       ├── vector/                  HNSWIndex (from-scratch approximate nearest neighbor)
│       ├── aggregation/             Aggregator (terms/stats/histogram, shard-local)
│       ├── document/                Document (JSON parsing + typed field access)
│       ├── index/                   InvertedIndex (the mutable memtable index)
│       ├── segment/                 Segment (immutable, mmap'd) + SegmentMerger (LSM compaction)
│       ├── shard/                   Shard — the big integration point (see §4.9)
│       ├── rpc/                     EngineServiceImpl — gRPC ↔ Shard translation
│       └── main.cpp                 engine node entry point + background maintenance thread
├── scripts/                         build.sh, run_cluster.sh, demo.sh, stop_cluster.sh
└── examples/sample_docs.jsonl       a small sample dataset
```

Every domain module under `engine/src/` follows the same pattern: a `.h`, a
`.cpp`, and a `_test.cpp` with a plain `assert()`-based `main()` (no test
framework dependency), wired into `CMakeLists.txt` as its own `ctest` target.

---

## 3. The gRPC contract (`proto/engine.proto`)

This is the **only** thing the Go and C++ halves of the system agree on.
Everything else is private to each side. The service is:

```protobuf
service EngineService {
  rpc IndexDocument(IndexRequest) returns (IndexResponse);
  rpc GetDocument(GetRequest) returns (GetResponse);
  rpc DeleteDocument(DeleteRequest) returns (DeleteResponse);
  rpc BulkIndex(BulkRequest) returns (BulkResponse);
  rpc Search(SearchRequest) returns (SearchResponse);
  rpc CreateShard(CreateShardRequest) returns (CreateShardResponse);
  rpc Flush(FlushRequest) returns (FlushResponse);
  rpc Merge(MergeRequest) returns (MergeResponse);
  rpc ShardStatus(ShardStatusRequest) returns (ShardStatusResponse);
  rpc Health(HealthRequest) returns (HealthResponse);
}
```

Design choices worth calling out:
- **Documents are raw JSON strings**, not typed protobuf messages — the
  document schema is user-defined (schemaless, like Elasticsearch), so the
  engine parses/analyzes fields according to the shard's field mapping (set
  once at `CreateShard` time) rather than the wire format constraining the
  document shape.
- **Filters, vector queries, and aggregations are explicit typed
  messages**, not a generic query-DSL blob — this keeps the C++ query
  planner in `Shard::Search` simple and makes the scatter-gather contract
  explicit and easy to reason about (no JSON-inside-JSON parsing on the hot
  path).
- Regenerating the stubs (after editing `proto/engine.proto`) is one
  `protoc` invocation per language; see the header comments in
  `coordinator/internal/genproto/` and `engine/genproto/` — the exact
  commands used are:
  ```bash
  # Go
  protoc --go_out=coordinator --go_opt=module=dse/coordinator \
         --go-grpc_out=coordinator --go-grpc_opt=module=dse/coordinator \
         -I proto proto/engine.proto
  # C++
  protoc -I proto --cpp_out=engine/genproto --grpc_out=engine/genproto \
         --plugin=protoc-gen-grpc=$(brew --prefix grpc)/bin/grpc_cpp_plugin \
         proto/engine.proto
  ```

---

## 4. C++ engine — module by module

### 4.1 `analysis/` — text analysis pipeline

`Tokenizer::Tokenize` splits raw text into word runs (alphanumeric plus
internal `'`/`-`, so `"don't"` and `"well-known"` survive as single tokens,
with leading/trailing marks trimmed), tagging each with its ordinal
`position`. `Analyzer::Analyze` composes tokenize → ASCII-lowercase → drop
if in a ~48-word English stopword list, returning the final ordered term
list **with duplicates preserved** (term frequency depends on it). This is
the "standard analyzer" every `text`-typed field is run through before
indexing and before every keyword search query.

### 4.2 `bitmap/` — Roaring Bitmap

A real, simplified Roaring Bitmap: each `uint32_t` is split into a 16-bit
high key and 16-bit low value; a `std::map<uint16_t, Container>` holds one
container per high-key "chunk," where a `Container` is either a sorted
`std::vector<uint16_t>` (sparse, ≤4096 values) or a `std::array<uint64_t,
1024>` bitset (dense, >4096 values), auto-promoted on `Add()`. `And`/`Or`/
`AndNot` operate chunk-by-chunk (only touching chunks that exist in the
relevant operand(s)), not as an O(2³²) full-space scan. This is what makes
multi-term query intersection and deleted-document exclusion fast — a
posting list for a common term might be millions of ids, and a bitset chunk
lets `Contains`/set-algebra run in O(1) per value instead of O(n) linear
scans. `Serialize`/`Deserialize` give a self-describing byte format used to
persist posting lists inside segment files (§13.1).

### 4.3 `wal/` — Write-Ahead Log

Every index/delete operation is **durably appended before it's applied to
memory**. Format: length-prefixed binary frames — `doc_id`, op type,
`json` payload (empty for deletes), and a **CRC32 checksum** over the whole
record — each `Append()` call `fsync`s before returning. `ReadAll()` reads
from byte 0 and stops cleanly (returning everything valid read so far,
without throwing) the instant it hits a torn/short record or a CRC
mismatch — this is what correctly models "the process crashed mid-write of
the last record" rather than corrupting recovery. `Truncate()` resets the
file to empty once a `Shard::Flush()` has durably captured the memtable's
contents in a segment.

### 4.4 `storage/` — Memory-mapped files

`MMapFile` is an RAII wrapper around POSIX `mmap`/`munmap`/`msync`.
`CreateAndMap` opens/truncates/maps a file `PROT_READ|PROT_WRITE,
MAP_SHARED` for writing (used by the segment writer to build files in
their final on-disk layout without a separate buffered-write pass);
`OpenReadOnly` maps an existing file `PROT_READ` (used by every segment
reader — this is the "zero-copy from disk" mechanism: the OS page cache
serves the bytes directly, no `read()` syscall loop copying into
application-managed heap memory). Movable, not copyable; the destructor
`munmap`s and closes exactly once, safe on a moved-from instance.

### 4.5 `scoring/` — BM25

`BM25Scorer::Score(term_freq, doc_length, avg_doc_length, total_docs,
doc_freq)` implements the exact Okapi BM25 formula used by
Elasticsearch/Lucene:

```
idf   = ln(1 + (N - df + 0.5) / (df + 0.5))          // smoothed, always ≥ 0
score = idf * (tf * (k1 + 1)) / (tf + k1 * (1 - b + b * (docLen / avgDocLen)))
```

with the standard defaults `k1 = 1.2`, `b = 0.75`. The `+1` inside the log
is what keeps `idf` non-negative even for a term present in every document
(the plain, unsmoothed BM25 IDF formula can go negative there, which is a
known footgun this variant avoids). Verified: term-frequency saturation
(diminishing returns), length normalization (longer docs score lower, all
else equal, when `b > 0`; `b = 0` disables length normalization exactly),
and IDF non-negativity across the full `doc_freq` range.

### 4.6 `vector/` — HNSW (Hierarchical Navigable Small World)

A real, from-scratch (simplified relative to production libraries like
`hnswlib`, but algorithmically genuine) approximate-nearest-neighbor index:

- **Level assignment**: `level = floor(-ln(uniform()) * mL)`, `mL = 1/ln(M)`
  (standard HNSW formula), using a fixed-seed `std::mt19937` for
  determinism.
- **Graph**: per-node, per-layer neighbor lists, up to `M` links per layer
  (`2M` at layer 0, the denser base layer, per the standard HNSW
  convention).
- **Insertion**: greedy single-best descent from the entry point down to the
  new node's top layer, then a real beam search with `ef_construction`
  candidates at each layer down to 0, connecting to the `M` closest
  candidates (a "keep M closest" simplification of the full HNSW heuristic,
  which also does neighbor diversification — noted in code comments) with
  bidirectional links and furthest-neighbor pruning on overflow.
- **Search**: the same greedy-then-beam-search shape, with `ef_search`
  candidates at layer 0.
- **Distance metrics**: cosine similarity and squared L2 distance, both
  exposed to callers as a similarity score (higher = better) — L2 distance
  is converted via `1/(1+dist)`.

Verified against a brute-force linear-scan reference on a 1500-vector,
32-dimension randomized (fixed-seed) dataset: **99–100% top-10 recall
overlap** with the exact brute-force answer, for both distance metrics.

### 4.7 `aggregation/` — shard-local faceting

`Aggregator::ComputeTerms/ComputeStats/ComputeHistogram` compute one
shard's contribution to a distributed aggregation, over a caller-supplied
set of matching document ids and a `ValueLookup` callback (`doc_id ->
optional<FieldValue>`, kept generic/decoupled from any specific document
storage). `ComputeTerms` counts and sorts distinct string values
(doc-count desc, key asc, truncated to `size`). `ComputeStats` computes
count/sum/min/max/avg over numeric values (correctly returns
all-zero/no-NaN on zero matching docs). `ComputeHistogram` buckets numeric
values into fixed-width intervals, sorted **numerically** by bucket key
(not lexicographically — `"5"` sorts before `"10"`, unlike a naive string
sort). These per-shard partial results are what `coordinator/internal/
routing/search.go`'s `mergeAggregations` combines into a single global
result (§8).

### 4.8 `document/`, `index/`, `segment/` — the storage core

- **`document/Document`** wraps a parsed `nlohmann::json` object with typed
  accessors (`GetString`, `GetNumber`, `GetVector`, and a lenient
  `GetAsIndexableString` that stringifies numbers/booleans/string-arrays
  for text/keyword indexing).
- **`index/InvertedIndex`** is the **mutable memtable index**: `field ->
  term -> Postings{doc_ids: RoaringBitmap, term_freqs: aligned vector}`,
  plus per-field document-length statistics for BM25. It is *append-only* —
  documents are never updated or removed in place (see §4.9's "update = new
  internal id" policy) — which is exactly what lets it be trivially
  snapshotted into an immutable on-disk segment.
- **`segment/Segment`** is a **write-once, read-many, memory-mapped**
  on-disk structure: `Segment::Write` serializes a snapshot of postings,
  per-field average lengths, doc-field-lengths, and stored document sources
  into a single binary file (format in §13.1); `Segment::Open` `mmap`s the
  file read-only and parses it **once** into the same in-memory map shapes
  `InvertedIndex` uses, so query code can treat "the memtable" and "any
  segment" uniformly through a shared `GetPostings`/`DocFieldLength`/
  `AvgFieldLength` interface. This is a deliberate, documented tradeoff — a
  "true" zero-copy design would traverse postings directly over the mapped
  bytes without ever materializing them in the process heap; here, opening
  a segment still gets the mmap-vs-`read()` win (no syscall-per-page copy
  loop, OS page cache does the work), but query-time postings access is
  against materialized RAM structures rather than the raw mapped bytes.
- **`segment/SegmentMerger`** implements LSM compaction: given N input
  segment paths and the shard's current tombstone set (a `RoaringBitmap` of
  deleted internal doc ids), it opens every input segment, keeps only
  surviving documents, recomputes per-field average lengths from survivors
  only, and writes one combined output segment. **This is the only point at
  which a deleted document's postings/stored data are physically
  reclaimed** — individual segments are never edited in place.

### 4.9 `shard/Shard` — the integration point

One `Shard` owns: one `WriteAheadLog`, one mutable `InvertedIndex` memtable
plus its doc list, zero-or-more immutable `Segment`s, a global tombstone
`RoaringBitmap`, and one in-memory `HNSWIndex` per `vector`-typed field. A
single `std::mutex` guards all mutable state — deliberately simple, not
lock-free or read/write-split (§16 discusses the production alternative).

**Document identity.** Every physically-indexed occurrence of a document
gets a fresh, monotonically increasing `uint32_t internal_id`, never reused.
"Updating" a document (re-indexing the same client-facing string `doc_id`)
is implemented as **tombstone the old internal id + index under a brand-new
internal id** — this is what lets `InvertedIndex` stay append-only and
`Segment` stay immutable; it's the same technique Lucene uses.

**Search** (`Shard::Search`, the single most involved method — see §7 and
§9 for the full walk-through) builds a live candidate `RoaringBitmap` from
either a keyword-term union (with inline BM25 accumulation) or match-all,
narrows it by term filters (postings AND) and range filters (post-filter by
reading stored source), optionally blends in HNSW vector-similarity scores
intersected with the same candidate set, sorts, paginates, and — over the
*full* filtered candidate set, not just the returned page — computes any
requested aggregations.

**Flush** (`Shard::Flush`) snapshots the current memtable into a new
immutable `Segment` file, clears the memtable, **persists the tombstone
bitmap to `tombstones.bin` before truncating the WAL** (so deletions of
already-flushed documents survive the WAL truncation), and truncates the
WAL. **Merge** (`Shard::Merge`) runs `SegmentMerger` across every current
segment, replaces them all with the one merged output, and deletes the old
segment files from disk.

**Startup / recovery** (`Shard`'s constructor): load `tombstones.bin` →
scan `data_dir_` for `segment_*.seg` files, `Segment::Open` each (in id
order, so `next_internal_id_` ends up past every id ever assigned) →
backfill vector indexes from segment-resident vector fields → replay the
WAL (each replayed record runs through the *exact same* in-memory-mutation
code path live indexing uses, incrementally repopulating the memtable,
tombstones, and vector index for anything written since the last flush).

---

## 5. Go coordinator — module by module

### 5.1 `internal/config` — CLI flags

`config.Load()` parses `-node-id`, `-http-addr`, `-raft-addr`, `-raft-dir`,
`-bootstrap`, `-join`, `-engine-nodes` (comma-separated), `-shards`,
`-replicas` into a `NodeConfig`, using a fresh `flag.FlagSet` per call (safe
to call more than once per process, which the test suite exercises).

### 5.2 `internal/api` — REST layer

Defines the JSON wire types (`SearchRequest`, `Aggregation`, `Filter`,
etc.) and the `Coordinator` interface — the **sole** dependency of the HTTP
handlers, implemented for real by `internal/routing.Router`. Routes (full
table in §14): index CRUD (`PUT/GET/DELETE /indices/{index}/_doc/{id}`),
`_bulk`, `_search`, index creation (`PUT /indices/{index}`), and
`GET /_cluster/health`. `404` for missing documents/indices, `400` for
malformed JSON, `500` for downstream errors, `application/json` throughout.

### 5.3 `internal/cluster` — Raft-replicated cluster metadata

`FSM` (implements `raft.FSM`) holds the single source of truth for shard
placement: `State{Indices map[string]*IndexMeta, EngineNodes []string}`.
Two command types flow through the Raft log:

- **`CmdCreateIndex`** — the command payload captures the engine-node pool
  **at the moment the leader builds the command** (not read fresh from
  local state at apply time), so shard placement is a pure function of the
  command's own data — every coordinator that replays this log entry
  computes the *identical* placement, which is what Raft's
  replicated-state-machine determinism requires. Placement itself:
  `primary = engineNodes[shardIndex % len(engineNodes)]`, replicas are the
  next `numReplicas` distinct engine nodes in round-robin order after the
  primary (capped at `len(engineNodes)-1`).
- **`CmdRegisterEngineNode`** — appends an engine node address to the known
  pool (idempotent).

`Node` wraps `*raft.Raft` plus BoltDB-backed log/stable stores and a
file-based snapshot store. `NewNode` either bootstraps a brand-new
single-node cluster (`-bootstrap`, only if no existing Raft state is found
on disk — safe to restart a bootstrap node without re-bootstrapping) or
starts as a raft participant awaiting either restart-recovery of existing
state or an explicit `Join` call from another node.

### 5.4 `internal/engineclient` — gRPC connection pool

`Pool` lazily dials and caches one `*grpc.ClientConn` per engine-node
address (`grpc.NewClient` with insecure transport credentials — see §16 for
the TLS gap), so repeated scatter-gather calls reuse a long-lived HTTP/2
connection rather than reconnecting every request. `grpc-go` client
connections automatically reconnect after a transient disconnect (verified
live: killing and restarting an engine node mid-demo required no
coordinator-side reconnection logic).

### 5.5 `internal/replication` — write fan-out

`IndexDocument`/`DeleteDocument`/`BulkIndex` send synchronously to a
shard's primary (the caller gets that response) and **asynchronously,
fire-and-forget, in a goroutine per replica** to every replica afterward.
`CreateShard` is the one exception — sent synchronously to primary *and*
every replica, since shard creation is rare/administrative and failures
should surface immediately rather than be discovered later during a
document write. See §15 for the full consistency-model discussion.

### 5.6 `internal/routing` — the scatter-gather coordinator

Implements `api.Coordinator`. The three interesting pieces:

- **Document routing** (`shardForKey`): FNV-1a hash of the (coordinator-
  generated-if-empty, via `uuid.NewString()`) `doc_id`, mod shard count —
  deterministic, so index/get/delete for the same key always land on the
  same shard.
- **`Search`** (§7): parallel per-shard gRPC fan-out via goroutines + a
  `sync.WaitGroup`, globally re-sorted/paginated hit merge, plus
  aggregation merging (§8).
- **`CreateIndex`**: commits shard placement to the Raft log (leader-only),
  then physically creates the shard on every engine node that owns a copy
  via `replication.CreateShard`.

---

## 6. Execution flow: indexing a document

```
1. Client: PUT /indices/products/_doc/p1  {title:"...", brand:"...", ...}
2. api.Handler.IndexDocument → Coordinator.IndexDocument(ctx, "products", "p1", body)
3. routing.Router.IndexDocument:
     meta := indexMeta("products")            // read current Raft-replicated shard list
     shard := meta.Shards[fnv1a("p1") % len(meta.Shards)]
     replication.IndexDocument(pool, shard, IndexRequest{...})
4. replication.IndexDocument:
     primary_client := pool.Get(shard.Primary)
     resp := primary_client.IndexDocument(ctx, req)      // SYNCHRONOUS — response returned to client
     for replica in shard.Replicas:
       go replica_client.IndexDocument(bg_ctx, req)       // ASYNC fire-and-forget
5. [on the primary engine node] EngineServiceImpl.IndexDocument
     → Shard::Index(doc_id, json)
        → Document::Parse(json)                            // validate BEFORE touching the WAL
        → wal_->Append({kIndex, doc_id, json})              // durable BEFORE applying to memory
        → ApplyIndexLocked(doc_id, json):
             - if doc_id already mapped: tombstone the OLD internal id
             - new_id = next_internal_id_++
             - for each field mapping:
                 text     -> Analyzer::Analyze -> InvertedIndex::IndexField
                 keyword  -> lowercase whole value -> IndexField (single token)
                 number   -> format as exact-match token -> IndexField (single token)
                 vector   -> HNSWIndex::Add(new_id, vector)
             - append to memtable doc list
        → returns {success, version=new_id}
6. Response propagates back up: engine -> coordinator -> client
```

## 7. Execution flow: scatter-gather search

```
1. Client: POST /indices/products/_search {"query":"running shoes", "fields":["title"], "size":10}
2. routing.Router.Search:
     meta := indexMeta("products")                          // e.g. 3 shards
     pbReq := toProtoSearchRequest(req)                       // per-shard Size = req.From+req.Size
                                                                // (must over-fetch — don't know which
                                                                //  shard holds the global top results)
     for each shard IN PARALLEL (goroutine + WaitGroup):
       resp := pool.Get(shard.Primary).Search(ctx, pbReq{ShardId: shard.ShardID})
     wait for all goroutines
3. [on EACH engine node, concurrently] Shard::Search:
     terms := Analyzer::Analyze("running shoes")  // ["running", "shoes"]
     for each field in ["title"]:
       for each term in terms:
         tm := TermMatchWithFreqsLocked(field, term)   // union of memtable + every segment's postings,
                                                          // already excludes tombstoned docs
         for each doc_id in tm.doc_ids:
           candidates.Add(doc_id)
           bm25_scores[doc_id] += BM25Scorer::Score(tm.freqs[doc_id], doc_length, avg_length, N, df)
     [apply term filters: candidates = AND(candidates, term_bitmap)]
     [apply range filters: post-filter by reading stored source]
     sort candidates by bm25_scores desc, doc_id asc (tiebreak)
     return top (from+size) hits + total_hits (= this shard's full candidate count)
4. Back in routing.Router.Search:
     allHits := concat every shard's returned hits             // shards are disjoint, no dedup needed
     sort allHits by score desc, doc_id asc                     // GLOBAL re-rank
     page := allHits[req.From : req.From+req.Size]              // GLOBAL pagination
     totalHits := sum of every shard's total_hits
     return SearchResponse{Hits: page, TotalHits: totalHits, TookMillis: elapsed}
```

A shard that fails to respond (unreachable node, timeout, error) is simply
**excluded** from the merge — the query returns degraded-but-successful
partial results rather than failing outright. This was verified live: with
one of two engine nodes `kill -9`'d, a 5-document match-all search
correctly returned exactly the 2 documents whose shards were still
reachable, `total_hits: 2`, HTTP 200 — not an error.

## 8. Execution flow: distributed aggregations

Aggregations are the hardest part of "distributed" search to get right,
because a shard only sees its own slice of the data.

```
1. Request: {"aggregations": [{"name":"by_brand","type":"terms","field":"brand","size":10}]}
2. routing.toProtoSearchRequest: OVER-FETCHES per shard —
     pbAgg.Size = requestedSize*3 + 10     // ask each shard for MORE buckets than the client wants
3. [on each shard] Aggregator::ComputeTerms over the shard's OWN filtered candidate set
     -> up to pbAgg.Size buckets, sorted doc_count desc
4. routing.mergeAggregations:
     for each shard's AggregationResult with the same `name`:
       for each bucket: globalBucketCounts[bucket.Key] += bucket.DocCount
     sort globalBucketCounts by count desc, key asc
     truncate to the CLIENT's originally-requested size
     return
```

The over-fetch (`size*3+10` buckets per shard instead of exactly `size`) is
a **documented approximation**, not a full fix — it's the same mitigation
real distributed search engines use, because the only way to get an exactly
correct global top-N terms aggregation without over-fetching is either
exhaustive per-shard bucket transmission (expensive) or a second
coordination round-trip (Elasticsearch's own docs call this out explicitly
via `sum_other_doc_count` — the count is a best-effort approximation, not a
guarantee, in any real distributed system too).

`stats` aggregations merge exactly (not approximately): sum/count are
additive across shards, min/max are taken globally, and **avg is always
recomputed as `sum/count` after merging** — never averaged-across-shard-
averages, which would silently double-weight small shards.

`histogram` aggregations merge like `terms` but sort by the bucket's
**numeric** value (not lexicographic string order — `"5"` sorts before
`"10"`), and are not size-truncated.

Live-verified: 5 documents split across 3 shards on 2 physical engine
nodes, `terms` aggregation on `brand` correctly returned the true global
counts (`Nike: 2, Adidas: 2, Puma: 1`) even though no single shard held
more than 2 of the documents.

## 9. Execution flow: hybrid vector + keyword search

```
1. Request: {"query":"shoes","fields":["title"],
             "vector":{"field":"embedding","embedding":[1,0,0,0],"k":5},
             "vector_weight":0.5}
2. [on each shard] Shard::Search:
     candidates, bm25_scores := <keyword search as in §7>
     [term/range filters applied to `candidates`]

     vector_hits := HNSWIndex::Search(embedding, k, ef_search)
     vector_scores := {}
     for (doc_id, similarity) in vector_hits:
       if candidates.Contains(doc_id):              // HARD constraint: vector hits must also
         vector_scores[doc_id] = similarity          // satisfy the keyword query + filters

     max_bm25 := max(bm25_scores[id] for id in candidates)   // normalize BM25 into [0,1]
     for id in candidates:
       keyword_component := bm25_scores[id] / max_bm25
       vector_component  := vector_scores.get(id, 0.0)
       final_score := (1 - vector_weight)*keyword_component + vector_weight*vector_component
```

Design decision: a hybrid query's vector-similarity signal is always
**intersected with** the keyword/filter candidate set, never a separate,
independently-ranked channel unioned in afterward — filters are hard
constraints regardless of which retrieval mechanism (BM25 or HNSW) surfaced
a document. BM25 raw scores are min-max-normalized (divided by the batch
max) only when blending with a vector score, so the two heterogeneous score
scales combine meaningfully; pure keyword search (no vector query) returns
raw, unnormalized BM25 scores, matching typical single-signal ranking
behavior. Live-verified: with `vector_weight: 1.0` (pure vector ranking)
against `[1,0,0,0]`, five documents with embeddings at varying cosine
distances from that query vector were ranked in exactly the expected
order, with the exact-direction match scoring `1.0` and the orthogonal
vectors scoring `0.0`.

## 10. Execution flow: LSM flush, merge, and background maintenance

```
main.cpp spawns a std::thread on startup:
  loop forever:
    sleep(maintenance_interval_sec)         // default 10s, configurable via --maintenance-interval-sec
    EngineServiceImpl::RunMaintenanceOnce():
      for each shard this node owns:
        if shard.memtable_doc_count > 0:
          shard->Flush()                     // memtable -> new immutable segment; WAL truncated
        if shard.segment_count >= 3:          // kMergeSegmentThreshold
          shard->Merge()                      // all current segments -> 1 merged segment,
                                                // tombstoned docs physically dropped
```

This is a **real background thread**, independent of and concurrent with
request handling (each `Shard`'s own mutex serializes its internal state,
so a flush/merge on one shard never blocks request handling on another, and
the maintenance thread releases its own shard-list lock before doing the
potentially-slow flush/merge work). The thresholds (10s interval, 3-segment
merge trigger) are intentionally small for demo/test visibility — see §16
for the production-tuning discussion.

## 11. Execution flow: crash recovery

This was validated with a genuine `kill -9` (no graceful shutdown, no
signal handler — the closest thing to an actual power-loss/OOM-kill
scenario a local test can produce) against a live, multi-shard, replicated
cluster in `scripts/demo.sh`. Two independent recovery mechanisms compose:

**Per-shard recovery** (`Shard`'s constructor, §4.9): replays the WAL,
reconstructing any memtable state that existed at crash time but hadn't yet
been flushed to a segment.

**Per-process recovery** (`EngineServiceImpl::LoadExistingShards`, called
once from the constructor): scans the engine's `data_dir` for
subdirectories containing a `mappings.json` (written by `CreateShard`
before constructing the in-memory `Shard` object) and reconstructs a
`Shard` for each one found. **This is necessary because `shards_` starts
empty on every process restart** — only a shard's own WAL/segment state is
recovered "for free" by `Shard`'s constructor; the *engine process* itself
has no memory of which shards it used to own unless it re-derives that from
disk. (This was an actual bug caught and fixed during development of this
system — an earlier version only implemented per-shard WAL replay and
silently returned "shard not found" for every request after a real process
restart, since `shards_` was empty; `LoadExistingShards` + persisting
`mappings.json` at `CreateShard` time closes that gap. See the git-history-
style narrative in this codebase's construction: this is exactly the kind
of gap that only surfaces under a *real* process-level crash test, not a
Shard-object-level unit test — which is why `scripts/demo.sh`'s crash-kill
step exists as a first-class part of the demo, not just the C++ unit
tests.)

## 12. Cluster membership: Raft bootstrap and join

Single-node "cluster" (the default demo mode): one coordinator started with
`-bootstrap` immediately forms a one-member Raft cluster and is its own
leader.

Multi-node cluster: start node 1 with `-bootstrap`; start additional nodes
**without** `-bootstrap` and either pass `-join=<node1-http-addr>` (which
triggers an automatic `POST /_cluster/join` request against that address on
startup) or issue that `POST /_cluster/join {"node_id":"...",
"raft_addr":"..."}` manually against whichever node is currently leader.
`cluster.Node.Join` calls `raft.AddVoter`, which only succeeds when invoked
on the actual leader — `IsLeader()`/`LeaderAddr()` are exposed via
`GET /_cluster/health` so a client (or `-join`'s retry logic) can find the
right node to talk to.

## 13. On-disk formats

### 13.1 Segment file format

A plain sequential binary format (little-endian), described byte-for-byte
in `engine/src/segment/segment.cpp`:

```
magic:            char[5]  "DSEG1"
field_count:       uint32
  per field:        field_name (len-prefixed string), avg_length (double)
term_count:        uint32
  per (field,term):  field, term (len-prefixed strings)
                      bitmap_bytes (len-prefixed RoaringBitmap::Serialize() output)
                      freq_count:uint32, freq_count × uint32   (term frequencies, aligned with
                                                                  the bitmap's ascending doc-id order)
doclen_count:      uint32
  per entry:         field, doc_id:uint32, length:uint32
doc_count:         uint32
  per doc:            internal_id:uint32, doc_id (len-prefixed string), source_json (len-prefixed string)
```

### 13.2 Write-ahead log format

Length-prefixed binary frames, described in §4.3 / `engine/src/wal/wal.h`:
`doc_id_len+doc_id`, `op_type:uint8`, `json_len+json`, `crc32:uint32`
(CRC32 computed over every preceding byte of the record).

### 13.3 `tombstones.bin`

Just a raw `RoaringBitmap::Serialize()` byte dump of the shard's current
deleted-internal-id set, rewritten on every `Flush()` (before the WAL is
truncated) and every `Merge()`.

### 13.4 `mappings.json`

A small JSON array (`[{"field":"title","type":"text","vector_dim":0}, ...]`)
written once per shard directory by `CreateShard`, read back by
`EngineServiceImpl::LoadExistingShards` on process restart (§11).

---

## 14. REST API reference

| Method | Path | Body | Notes |
|---|---|---|---|
| `PUT` | `/indices/{index}` | `{"mappings":[...], "shards":N, "replicas":N}` | Commits shard placement via Raft, then creates the shard on every owning engine node |
| `PUT` | `/indices/{index}/_doc/{id}` | raw document JSON | Always `201`; an "update" is transparent (old version tombstoned) |
| `GET` | `/indices/{index}/_doc/{id}` | — | `404` if not found (still a valid, non-error response) |
| `DELETE` | `/indices/{index}/_doc/{id}` | — | `200 {found:true}` or `404 {found:false}` |
| `POST` | `/indices/{index}/_bulk` | `{"ops":[{"type":"index"\|"delete","doc_id":"...","body":{...}}]}` | `200` even if individual ops fail — check `results[i].success` |
| `POST` | `/indices/{index}/_search` | `SearchRequest` JSON (empty body `{}` = match-all) | See `internal/api/types.go` for the full shape |
| `GET` | `/_cluster/health` | — | Node id, leader status, peer list, full shard placement |
| `POST` | `/_cluster/join` | `{"node_id":"...","raft_addr":"..."}` | Raft-membership plumbing, not a Coordinator-interface route |
| `GET` | `/health` | — | Liveness only |

`SearchRequest` fields: `query` (string), `fields` (string array),
`filter.term`/`filter.range`, `vector.field`/`vector.embedding`/`vector.k`,
`vector_weight` (float), `aggregations` (array of
`{name,type,field,size,interval}` where `type` is `"terms"`, `"stats"`, or
`"histogram"`), `size`, `from`.

---

## 15. Consistency model

This is deliberately simple, and deliberately documented as such rather
than oversold:

- **Writes**: synchronous to the shard's primary (the client's response
  reflects the primary's outcome), asynchronous best-effort fan-out to
  replicas. This gives **read-your-writes consistency against the
  primary**, but only **eventual consistency on replicas** — if a replica
  is briefly unreachable when a write fans out, it silently lags until the
  next successful write to that shard reaches it (there is no read-repair
  or anti-entropy background sync in this implementation; see §16).
- **Reads**: `GET` always targets a shard's primary (not a random replica),
  so there's no risk of reading stale replica state on a direct
  document-id lookup.
- **Search**: scatter-gathers against every shard's primary. A shard whose
  primary is unreachable is dropped from the result set rather than
  failing the whole query (§7) — this is an availability/consistency
  tradeoff (AP-leaning for search availability) matching how most
  real-world distributed search engines behave by default.
- **Cluster metadata** (index existence, shard placement): strongly
  consistent via Raft — every coordinator node that has caught up on the
  Raft log has an identical view. `CreateIndex` must be issued against the
  current Raft leader (a non-leader returns a clear error naming the
  current leader rather than silently failing or forwarding).

## 16. Known simplifications and honest gaps

This system prioritizes **breadth of genuinely-working functionality** over
production-hardening every corner. Specific, named gaps:

- **Replication is fire-and-forget, not quorum-based.** A production system
  would want synchronous quorum writes or a per-shard consensus/replication
  log (treating each shard as its own small Raft group) for strict
  durability guarantees across replica loss. There is no read-repair or
  background anti-entropy sync for replicas that missed writes while
  unreachable.
- **No TLS.** `engineclient.Pool` dials with `insecure.NewCredentials()`;
  the coordinator's REST API is plain HTTP. Fine for local/trusted-network
  demos, not for production.
- **`Shard` uses one coarse-grained mutex**, not a lock-free or
  read/write-split design — under heavy concurrent load this serializes
  all reads and writes to a shard. A production system would likely use an
  RCU-style "build the next generation of segment list, atomically swap
  the pointer" pattern so readers never block on writers.
- **Range filters are post-filter, not an index structure.** There's no
  on-disk numeric range index (e.g., a BKD-tree, as Lucene uses) — a range
  filter reads and JSON-parses every current keyword/match-all candidate's
  stored source and checks bounds in memory. Fine at demo/moderate scale;
  would need a real range index at large document counts.
- **Distributed `terms`/`histogram` aggregations are approximate**, not
  exact, under the standard over-fetch mitigation (§8) — this is the same
  characteristic real distributed search engines have, not unique to this
  implementation, but worth restating plainly.
- **Vector indexes are not segmented or persisted to disk directly** — one
  `HNSWIndex` per vector field lives entirely in RAM per shard, rebuilt at
  startup by replaying the WAL and scanning segment-stored document
  sources (§4.9, §11). A production system would want the vector index
  itself to be part of the immutable segment format (with its own
  merge/compaction story), avoiding an O(all live docs) rebuild on every
  restart.
- **No graceful shutdown** in either the C++ engine or Go coordinator —
  both just run until killed. The background maintenance thread is
  `join()`'d on graceful `grpc::Server::Wait()` return, but there's no
  SIGTERM handler triggering that path.
- **Background maintenance thresholds are demo-tuned** (10s interval,
  merge at 3 segments), not production-tuned (which would more likely
  trigger primarily on segment *size*/*count* thresholds with a much
  longer or adaptive time interval, to bound both write amplification and
  staleness).
- **No authentication/authorization** anywhere in the REST or gRPC surface.
- **Shard rebalancing is not implemented** — shard-to-engine-node placement
  is decided once, at `CreateIndex` time, and never moves automatically if
  an engine node joins/leaves the pool later. Adding a new engine node
  requires creating a *new* index to make use of it (or a manual
  data-migration tool, not built here).

## 17. Testing strategy

- **C++ (`engine/`)**: every module has its own `_test.cpp` (plain
  `assert()`, no framework dependency) wired as its own `ctest` target —
  `analysis`, `bitmap`, `wal`, `storage`, `scoring`, `vector`,
  `aggregation`, `index`, `segment`, and a comprehensive `shard`
  integration suite (11 scenarios: CRUD, bulk, BM25 ranking, filters,
  pagination, aggregations, hybrid vector search, cross-segment search
  after flush, merge-reclaims-deleted-docs, WAL-replay crash recovery, and
  vector-index-survives-restart). Run via `cmake --build build && cd build
  && ctest --output-on-failure`, or `./scripts/build.sh`.
- **Go (`coordinator/`)**: `internal/config` and `internal/api` have unit
  tests (13 test functions total) — `internal/api`'s use a fake in-memory
  `Coordinator` implementation and `net/http/httptest` to exercise every
  REST route's happy path, not-found path, and malformed-input path. Run
  via `go test ./...`.
- **End-to-end** (`scripts/demo.sh`): the only way to actually validate
  cross-process, cross-language, cross-network behavior — starts a real
  2-engine-node + 1-coordinator cluster, exercises every feature over real
  HTTP + gRPC, and includes a genuine `kill -9` + restart to prove crash
  recovery works at the full-system level, not just inside a single
  in-process unit test. This is also how the `LoadExistingShards` gap
  (§11) was actually caught — it was invisible to `Shard`-level unit tests
  because those never destroy and recreate the *owning process*.

## 18. Extending the system

Some natural next steps, in rough order of "bang for the buck":

1. **Expose `Flush`/`Merge`/`ShardStatus` through the coordinator's REST
   API** (they already exist as gRPC RPCs and are used internally by the
   background maintenance thread and unit tests — just not reachable from
   the outside via HTTP yet).
2. **Shard rebalancing**: a coordinator-driven background job that,
   given an updated `EngineNodes` list, computes a new placement and
   migrates shard data (stream segment files + tail the WAL) to the new
   owner before cutting over.
3. **Synchronous quorum writes**: change `replication.IndexDocument` to
   wait for `W` of `N` replicas before returning success, with a
   configurable consistency level per request (matching Cassandra/Dynamo-
   style tunable consistency).
4. **Persist the vector index as part of the segment format** instead of
   rebuilding it in RAM at startup, with its own merge story.
5. **A real numeric range index** (a simple sorted-doc-id-per-bucket
   structure, or a small BKD-tree) to replace the post-filter range-filter
   implementation.

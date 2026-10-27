# Distributed Search Engine test documentation

Date: 2026-10-04 (Asia/Kolkata)

## Test layout

The added C++ tests are isolated in `engine/tests/edge_cases_test.cpp` and are
registered as a separate CTest target. Go tests remain beside the packages they
exercise so they can access package-private routing/API helpers without
changing production visibility:

- `engine/tests/edge_cases_test.cpp` — document validation and typed accessors,
  bitmap boundaries/dense containers/set algebra/serialization, WAL recovery
  and truncation, mmap move/read-only behavior, inverted-index statistics, and
  BM25 edge relationships.
- `coordinator/internal/routing/edge_cases_test.go` — REST-to-protobuf search
  translation, global pagination sizing, filter/vector translation, distributed
  terms/stats/histogram aggregation merging, and numeric bucket ordering.
- `coordinator/internal/api/edge_cases_test.go` — malformed JSON rejection,
  default search sizing, and coordinator-error-to-HTTP status mapping.

## Test forms and coverage

The project now exercises multiple layers without requiring a running cluster:

1. C++ unit-style executable tests for low-level storage, analysis, indexing,
   scoring, and serialization invariants.
2. C++ integration tests for shard CRUD/search/filters/vector search,
   aggregation, pagination, flush/segments, and recovery behavior.
3. Go HTTP handler tests using `httptest` and a fake coordinator.
4. Go routing tests for request translation and cross-shard aggregation merges.
5. Go race-detector testing for coordinator packages.
6. Persistence resilience checks using deliberately incomplete WAL tails and
   local mmap-backed files.

## Commands and results

### Go coordinator

```text
go test ./...
```

Result: **17 test functions passed, 0 failed**. Packages without test files
were also compiled successfully.

```text
go test -race ./...
```

Result: **17 test functions passed, 0 failed**, with no race reports.

### C++ engine

The repository's checked-in `engine/build` CTest metadata references an old
absolute checkout path (`/Users/krishnaditya/Documents/ideas/...`), so running
CTest directly there reports all tests as `Not Run` because the executables are
not found. The existing build directory was not deleted or modified. A fresh
build directory was configured instead:

```text
cmake -S engine -B engine/build-tests -DCMAKE_BUILD_TYPE=Release
cmake --build engine/build-tests --parallel 4
ctest --test-dir engine/build-tests --output-on-failure
```

Results:

- Fresh CMake configuration: passed.
- Fresh C++ build: passed, including `test_edge_cases`.
- CTest: **11/11 targets passed, 0 failed**.
- Total CTest wall time: approximately 4.61 seconds.

The 11 targets are the original 10 C++ test executables plus the new
`test_edge_cases` target.

## Infrastructure assessment

No Docker container, database, cache, message broker, cloud service, Raft
cluster, engine node, or external network service was started or required.
The tests use only:

- local C++ compilation and CTest;
- local Go compilation and `httptest` request/response objects;
- temporary files under `/tmp` for WAL/mmap tests;
- in-memory fakes and generated protobuf messages.

The project has scripts for multi-process engine/coordinator operation and a
crash-recovery demo, but those are heavier end-to-end infrastructure tests and
were intentionally not started in this run.

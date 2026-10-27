cmake_minimum_required(VERSION 3.20)
project(dse_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

# Homebrew keg-only packages: point CMake at their prefixes so find_package
# can locate the CONFIG-mode packages shipped by grpc/protobuf/abseil.
execute_process(COMMAND brew --prefix grpc OUTPUT_VARIABLE BREW_GRPC_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND brew --prefix protobuf OUTPUT_VARIABLE BREW_PROTOBUF_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND brew --prefix abseil OUTPUT_VARIABLE BREW_ABSEIL_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
list(APPEND CMAKE_PREFIX_PATH "${BREW_GRPC_PREFIX}" "${BREW_PROTOBUF_PREFIX}" "${BREW_ABSEIL_PREFIX}")

find_package(Threads REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Generated protobuf/gRPC code (checked in under genproto/, regenerated via
# `make proto` at the repo root — see scripts/gen_proto.sh)
# ---------------------------------------------------------------------------
add_library(dse_proto STATIC
  genproto/engine.pb.cc
  genproto/engine.grpc.pb.cc
)
target_include_directories(dse_proto PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/genproto
)
target_link_libraries(dse_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)

# ---------------------------------------------------------------------------
# Core engine library — every subsystem except main()/RPC glue, so it can be
# linked into both the real server binary and small standalone test binaries.
# ---------------------------------------------------------------------------
add_library(dse_core STATIC
  src/analysis/tokenizer.cpp
  src/analysis/analyzer.cpp
  src/bitmap/roaring_bitmap.cpp
  src/wal/wal.cpp
  src/storage/mmap_file.cpp
  src/scoring/bm25.cpp
  src/vector/hnsw.cpp
  src/aggregation/aggregator.cpp
  src/document/document.cpp
  src/index/inverted_index.cpp
  src/segment/segment.cpp
  src/segment/segment_merger.cpp
  src/shard/shard.cpp
)
target_include_directories(dse_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(dse_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# Server binary
# ---------------------------------------------------------------------------
add_executable(dse_engine src/main.cpp src/rpc/engine_service.cpp)
target_link_libraries(dse_engine PRIVATE dse_core dse_proto gRPC::grpc++)

# ---------------------------------------------------------------------------
# Lightweight self-tests (plain assert()-based, run via `ctest` or directly)
# ---------------------------------------------------------------------------
enable_testing()
function(dse_test name src)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE dse_core)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

dse_test(test_analysis      src/analysis/analyzer_test.cpp)
dse_test(test_bitmap        src/bitmap/roaring_bitmap_test.cpp)
dse_test(test_wal           src/wal/wal_test.cpp)
dse_test(test_storage       src/storage/mmap_file_test.cpp)
dse_test(test_scoring       src/scoring/bm25_test.cpp)
dse_test(test_vector        src/vector/hnsw_test.cpp)
dse_test(test_aggregation   src/aggregation/aggregator_test.cpp)
dse_test(test_index         src/index/inverted_index_test.cpp)
dse_test(test_segment       src/segment/segment_test.cpp)
dse_test(test_shard         src/shard/shard_test.cpp)
dse_test(test_edge_cases    tests/edge_cases_test.cpp)

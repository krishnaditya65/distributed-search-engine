// engine_service.h — the gRPC surface (EngineService, defined in
// proto/engine.proto) that the Go coordinator talks to. This is a thin
// translation layer: protobuf message <-> dse::shard::Shard call. All real
// logic lives in Shard; this file's only job is (de)serialization and
// managing the set of shards this engine node currently owns.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <grpcpp/grpcpp.h>

#include "engine.grpc.pb.h"
#include "shard/shard.h"

namespace dse::rpc {

class EngineServiceImpl final : public dse::EngineService::Service {
 public:
  // `data_dir` is the parent directory under which each shard gets its own
  // subdirectory (data_dir/<shard_id>/).
  explicit EngineServiceImpl(std::string data_dir, std::string node_id);

  grpc::Status IndexDocument(grpc::ServerContext* ctx, const dse::IndexRequest* req,
                             dse::IndexResponse* resp) override;
  grpc::Status GetDocument(grpc::ServerContext* ctx, const dse::GetRequest* req,
                           dse::GetResponse* resp) override;
  grpc::Status DeleteDocument(grpc::ServerContext* ctx, const dse::DeleteRequest* req,
                              dse::DeleteResponse* resp) override;
  grpc::Status BulkIndex(grpc::ServerContext* ctx, const dse::BulkRequest* req,
                         dse::BulkResponse* resp) override;
  grpc::Status Search(grpc::ServerContext* ctx, const dse::SearchRequest* req,
                     dse::SearchResponse* resp) override;
  grpc::Status CreateShard(grpc::ServerContext* ctx, const dse::CreateShardRequest* req,
                           dse::CreateShardResponse* resp) override;
  grpc::Status Flush(grpc::ServerContext* ctx, const dse::FlushRequest* req,
                     dse::FlushResponse* resp) override;
  grpc::Status Merge(grpc::ServerContext* ctx, const dse::MergeRequest* req,
                     dse::MergeResponse* resp) override;
  grpc::Status ShardStatus(grpc::ServerContext* ctx, const dse::ShardStatusRequest* req,
                           dse::ShardStatusResponse* resp) override;
  grpc::Status Health(grpc::ServerContext* ctx, const dse::HealthRequest* req,
                     dse::HealthResponse* resp) override;

  // Runs one pass of background LSM maintenance across every shard this
  // node owns: flushes any shard with a non-empty memtable (so writes
  // become durable in an immutable segment promptly, not just left sitting
  // in the WAL indefinitely), then merges any shard whose segment count has
  // grown past kMergeSegmentThreshold (bounding read amplification and
  // reclaiming space from deleted/updated documents). Intended to be called
  // repeatedly from a dedicated background thread — see main.cpp — mirroring
  // "a background thread continuously merges segments" from the design
  // spec. Safe to call concurrently with live request handling: all shard
  // mutation is internally synchronized (see dse::shard::Shard).
  void RunMaintenanceOnce();

 private:
  static constexpr int32_t kMergeSegmentThreshold = 3;

  dse::shard::Shard* FindShard(const std::string& shard_id);

  // Persists `mappings` as data_dir_/<shard_id>/mappings.json so that a
  // freshly-started engine process (after a restart or crash) can
  // rediscover this shard and reconstruct it with the SAME field mappings
  // it was created with — without this, only in-process WAL replay would
  // work (see dse::shard::Shard), but the shard would never be re-added to
  // `shards_` at all after a process restart, since CreateShard is normally
  // only ever called once, at index-creation time, by the coordinator.
  void PersistShardMappings(const std::string& shard_id,
                             const std::vector<dse::document::FieldMapping>& mappings);

  // Scans data_dir_ for subdirectories containing a mappings.json (i.e.
  // pre-existing shards from a prior run) and reconstructs a dse::shard::Shard
  // for each one, which in turn replays its own WAL and re-opens its own
  // on-disk segments. Called once from the constructor. This is the engine
  // PROCESS's half of crash recovery; dse::shard::Shard's WAL replay is the
  // per-shard half.
  void LoadExistingShards();

  std::string data_dir_;
  std::string node_id_;
  std::chrono::steady_clock::time_point start_time_;

  std::mutex shards_mu_;
  std::unordered_map<std::string, std::unique_ptr<dse::shard::Shard>> shards_;
};

}  // namespace dse::rpc

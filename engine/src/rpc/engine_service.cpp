#include "rpc/engine_service.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "json.hpp"

namespace dse::rpc {

namespace fs = std::filesystem;
using dse::shard::Shard;

EngineServiceImpl::EngineServiceImpl(std::string data_dir, std::string node_id)
    : data_dir_(std::move(data_dir)),
      node_id_(std::move(node_id)),
      start_time_(std::chrono::steady_clock::now()) {
  fs::create_directories(data_dir_);
  LoadExistingShards();
}

void EngineServiceImpl::PersistShardMappings(
    const std::string& shard_id, const std::vector<dse::document::FieldMapping>& mappings) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& m : mappings) {
    arr.push_back({{"field", m.field}, {"type", m.type}, {"vector_dim", m.vector_dim}});
  }
  std::string path = data_dir_ + "/" + shard_id + "/mappings.json";
  fs::create_directories(data_dir_ + "/" + shard_id);
  std::ofstream out(path, std::ios::trunc);
  out << arr.dump();
}

void EngineServiceImpl::LoadExistingShards() {
  if (!fs::exists(data_dir_)) return;
  for (const auto& entry : fs::directory_iterator(data_dir_)) {
    if (!entry.is_directory()) continue;
    std::string shard_id = entry.path().filename().string();
    std::string mappings_path = entry.path().string() + "/mappings.json";
    if (!fs::exists(mappings_path)) continue;  // not a shard directory (or predates this feature)

    std::ifstream in(mappings_path);
    std::ostringstream oss;
    oss << in.rdbuf();
    std::vector<dse::document::FieldMapping> mappings;
    try {
      auto arr = nlohmann::json::parse(oss.str());
      for (const auto& m : arr) {
        dse::document::FieldMapping fm;
        fm.field = m.value("field", "");
        fm.type = m.value("type", "");
        fm.vector_dim = m.value("vector_dim", 0);
        mappings.push_back(fm);
      }
    } catch (const std::exception&) {
      continue;  // corrupt mappings.json — skip this shard rather than crash startup
    }

    std::lock_guard<std::mutex> lock(shards_mu_);
    shards_[shard_id] = std::make_unique<Shard>(shard_id, entry.path().string(), mappings);
  }
}

Shard* EngineServiceImpl::FindShard(const std::string& shard_id) {
  std::lock_guard<std::mutex> lock(shards_mu_);
  auto it = shards_.find(shard_id);
  if (it == shards_.end()) return nullptr;
  return it->second.get();
}

// ---------------------------------------------------------------------------
// Document CRUD
// ---------------------------------------------------------------------------

grpc::Status EngineServiceImpl::IndexDocument(grpc::ServerContext*, const dse::IndexRequest* req,
                                              dse::IndexResponse* resp) {
  Shard* shard = FindShard(req->shard_id());
  if (!shard) {
    resp->set_success(false);
    resp->set_error("shard not found: " + req->shard_id());
    return grpc::Status::OK;
  }
  auto index_result = shard->Index(req->document().id(), req->document().json());
  resp->set_success(index_result.success);
  resp->set_error(index_result.error);
  resp->set_version(index_result.version);
  return grpc::Status::OK;
}

grpc::Status EngineServiceImpl::GetDocument(grpc::ServerContext*, const dse::GetRequest* req,
                                            dse::GetResponse* resp) {
  Shard* shard = FindShard(req->shard_id());
  if (!shard) {
    resp->set_found(false);
    return grpc::Status::OK;
  }
  auto get_result = shard->Get(req->doc_id());
  resp->set_found(get_result.found);
  if (get_result.found) {
    auto* doc = resp->mutable_document();
    doc->set_id(req->doc_id());
    doc->set_json(get_result.json);
  }
  return grpc::Status::OK;
}

grpc::Status EngineServiceImpl::DeleteDocument(grpc::ServerContext*, const dse::DeleteRequest* req,
                                               dse::DeleteResponse* resp) {
  Shard* shard = FindShard(req->shard_id());
  if (!shard) {
    resp->set_success(false);
    resp->set_found(false);
    return grpc::Status::OK;
  }
  auto delete_result = shard->Delete(req->doc_id());
  resp->set_success(true);
  resp->set_found(delete_result.found);
  return grpc::Status::OK;
}

grpc::Status EngineServiceImpl::BulkIndex(grpc::ServerContext*, const dse::BulkRequest* req,
                                          dse::BulkResponse* resp) {
  Shard* shard = FindShard(req->shard_id());
  if (!shard) {
    for (int i = 0; i < req->ops_size(); i++) {
      auto* err_result = resp->add_results();
      err_result->set_success(false);
      err_result->set_error("shard not found: " + req->shard_id());
    }
    return grpc::Status::OK;
  }

  std::vector<dse::shard::BulkOpInput> ops;
  ops.reserve(req->ops_size());
  for (const auto& op : req->ops()) {
    dse::shard::BulkOpInput in;
    in.is_delete = (op.type() == dse::BulkOp::DELETE);
    in.doc_id = op.doc_id();
    in.json = op.json();
    ops.push_back(std::move(in));
  }
  auto results = shard->Bulk(ops);
  for (const auto& bulk_result : results) {
    auto* out = resp->add_results();
    out->set_success(bulk_result.success);
    out->set_error(bulk_result.error);
    out->set_doc_id(bulk_result.doc_id);
  }
  return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

grpc::Status EngineServiceImpl::Search(grpc::ServerContext*, const dse::SearchRequest* req,
                                       dse::SearchResponse* resp) {
  Shard* shard = FindShard(req->shard_id());
  if (!shard) {
    resp->set_success(false);
    resp->set_error("shard not found: " + req->shard_id());
    return grpc::Status::OK;
  }

  dse::shard::SearchParamsQ params;
  params.query_text = req->query_text();
  for (const auto& f : req->fields()) params.fields.push_back(f);

  if (req->has_filter()) {
    for (const auto& tf : req->filter().term()) {
      params.term_filters.push_back({tf.field(), tf.value()});
    }
    for (const auto& rf : req->filter().range()) {
      dse::shard::RangeFilterQ range_filter;
      range_filter.field = rf.field();
      range_filter.has_gte = rf.has_gte();
      range_filter.gte = rf.has_gte() ? rf.gte() : 0.0;
      range_filter.has_lte = rf.has_lte();
      range_filter.lte = rf.has_lte() ? rf.lte() : 0.0;
      params.range_filters.push_back(range_filter);
    }
  }

  if (req->has_vector() && !req->vector().field().empty()) {
    params.has_vector = true;
    params.vector.field = req->vector().field();
    params.vector.k = req->vector().k() > 0 ? req->vector().k() : 10;
    for (float v : req->vector().embedding()) params.vector.embedding.push_back(v);
  }
  params.vector_weight = req->vector_weight() > 0 ? req->vector_weight() : 0.5;

  for (const auto& agg : req->aggregations()) {
    dse::shard::AggregationQ agg_q;
    agg_q.name = agg.name();
    agg_q.type = agg.type();
    agg_q.field = agg.field();
    agg_q.size = agg.size() > 0 ? agg.size() : 10;
    agg_q.interval = agg.interval() > 0 ? agg.interval() : 1.0;
    params.aggregations.push_back(agg_q);
  }

  params.size = req->size() > 0 ? req->size() : 10;
  params.from = req->from();

  auto result = shard->Search(params);

  resp->set_success(result.success);
  resp->set_error(result.error);
  resp->set_total_hits(result.total_hits);
  resp->set_took_micros(result.took_micros);
  for (const auto& hit : result.hits) {
    auto* hit_out = resp->add_hits();
    hit_out->set_doc_id(hit.doc_id);
    hit_out->set_score(hit.score);
    hit_out->set_json(hit.json);
  }
  for (const auto& agg : result.aggregations) {
    auto* agg_out = resp->add_aggregations();
    agg_out->set_name(agg.name);
    if (agg.type == "stats") {
      auto* stats_out = agg_out->mutable_stats();
      stats_out->set_count(agg.stats.count);
      stats_out->set_sum(agg.stats.sum);
      stats_out->set_min(agg.stats.min);
      stats_out->set_max(agg.stats.max);
      stats_out->set_avg(agg.stats.avg);
    } else {
      for (const auto& bucket_in : agg.buckets) {
        auto* bucket = agg_out->add_buckets();
        bucket->set_key(bucket_in.key);
        bucket->set_doc_count(bucket_in.doc_count);
      }
    }
  }
  return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// Shard administration
// ---------------------------------------------------------------------------

grpc::Status EngineServiceImpl::CreateShard(grpc::ServerContext*, const dse::CreateShardRequest* req,
                                            dse::CreateShardResponse* resp) {
  std::lock_guard<std::mutex> lock(shards_mu_);
  if (shards_.count(req->shard_id())) {
    // Idempotent: creating an already-existing shard is a success no-op
    // (the coordinator may retry CreateShard calls after a partial failure).
    resp->set_success(true);
    return grpc::Status::OK;
  }
  std::vector<dse::document::FieldMapping> mappings;
  for (const auto& m : req->mappings()) {
    mappings.push_back({m.field(), m.type(), m.vector_dim()});
  }
  try {
    std::string shard_dir = data_dir_ + "/" + req->shard_id();
    // Persist mappings BEFORE constructing the Shard so that even if the
    // process crashes immediately after, LoadExistingShards() on next
    // startup will find a mappings.json and correctly rediscover this shard
    // (whatever partial WAL/segment state Shard's own constructor recovers
    // from is exactly the same either way).
    PersistShardMappings(req->shard_id(), mappings);
    shards_[req->shard_id()] = std::make_unique<Shard>(req->shard_id(), shard_dir, mappings);
    resp->set_success(true);
  } catch (const std::exception& e) {
    resp->set_success(false);
    resp->set_error(e.what());
  }
  return grpc::Status::OK;
}

grpc::Status EngineServiceImpl::Flush(grpc::ServerContext*, const dse::FlushRequest* req,
                                      dse::FlushResponse* resp) {
  Shard* shard = FindShard(req->shard_id());
  if (!shard) {
    resp->set_success(false);
    resp->set_error("shard not found: " + req->shard_id());
    return grpc::Status::OK;
  }
  auto flush_result = shard->Flush();
  resp->set_success(flush_result.success);
  resp->set_error(flush_result.error);
  resp->set_segment_id(flush_result.segment_id);
  return grpc::Status::OK;
}

grpc::Status EngineServiceImpl::Merge(grpc::ServerContext*, const dse::MergeRequest* req,
                                      dse::MergeResponse* resp) {
  Shard* shard = FindShard(req->shard_id());
  if (!shard) {
    resp->set_success(false);
    resp->set_error("shard not found: " + req->shard_id());
    return grpc::Status::OK;
  }
  auto merge_result = shard->Merge();
  resp->set_success(merge_result.success);
  resp->set_error(merge_result.error);
  resp->set_segments_merged(merge_result.segments_merged);
  resp->set_result_segment_id(merge_result.result_segment_id);
  return grpc::Status::OK;
}

grpc::Status EngineServiceImpl::ShardStatus(grpc::ServerContext*, const dse::ShardStatusRequest* req,
                                            dse::ShardStatusResponse* resp) {
  Shard* shard = FindShard(req->shard_id());
  if (!shard) {
    resp->set_exists(false);
    return grpc::Status::OK;
  }
  auto status = shard->Status();
  resp->set_exists(true);
  resp->set_doc_count(status.doc_count);
  resp->set_deleted_count(status.deleted_count);
  resp->set_segment_count(status.segment_count);
  resp->set_wal_size_bytes(status.wal_size_bytes);
  resp->set_memtable_doc_count(status.memtable_doc_count);
  return grpc::Status::OK;
}

grpc::Status EngineServiceImpl::Health(grpc::ServerContext*, const dse::HealthRequest*,
                                       dse::HealthResponse* resp) {
  resp->set_healthy(true);
  resp->set_node_id(node_id_);
  {
    std::lock_guard<std::mutex> lock(shards_mu_);
    for (const auto& [id, _] : shards_) resp->add_shard_ids(id);
  }
  auto uptime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time_);
  resp->set_uptime_seconds(uptime.count());
  return grpc::Status::OK;
}

void EngineServiceImpl::RunMaintenanceOnce() {
  // Snapshot the current shard pointers under the lock, then do the actual
  // (potentially slow) flush/merge work outside the lock so a long-running
  // merge on one shard never blocks CreateShard/new-shard registration for
  // others. Individual Shard objects remain safe to call concurrently with
  // request handling because Shard guards its own state with its own mutex.
  std::vector<Shard*> snapshot;
  {
    std::lock_guard<std::mutex> lock(shards_mu_);
    snapshot.reserve(shards_.size());
    for (auto& [id, shard] : shards_) snapshot.push_back(shard.get());
  }

  for (Shard* shard : snapshot) {
    auto status = shard->Status();
    if (status.memtable_doc_count > 0) {
      shard->Flush();
    }
    status = shard->Status();
    if (status.segment_count >= kMergeSegmentThreshold) {
      shard->Merge();
    }
  }
}

}  // namespace dse::rpc

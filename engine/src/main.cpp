// main.cpp — the C++ engine (data node) process entry point. Starts a gRPC
// server exposing EngineService (proto/engine.proto) on the configured port.
// One process = one engine node = a home for zero or more shards, created
// on demand by the Go coordinator via CreateShard RPCs.
//
// Usage:
//   dse_engine --port=7001 --data-dir=./data/engine1 --node-id=engine-1
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "rpc/engine_service.h"

namespace {

std::string GetArg(int argc, char** argv, const std::string& name, const std::string& def) {
  std::string prefix = "--" + name + "=";
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
  }
  return def;
}

}  // namespace

int main(int argc, char** argv) {
  std::string port = GetArg(argc, argv, "port", "7001");
  std::string data_dir = GetArg(argc, argv, "data-dir", "./data/engine");
  std::string node_id = GetArg(argc, argv, "node-id", "engine-" + port);
  int maintenance_interval_sec = std::atoi(GetArg(argc, argv, "maintenance-interval-sec", "10").c_str());

  std::string server_address = "0.0.0.0:" + port;

  dse::rpc::EngineServiceImpl service(data_dir, node_id);

  // The background LSM maintenance thread: this is the "background thread
  // continuously merges smaller segments into larger ones" mechanism from
  // the design spec. It periodically flushes each shard's memtable to a new
  // immutable segment and merges segments once a shard accumulates enough
  // of them, entirely independent of and concurrent with request handling.
  std::atomic<bool> stop_maintenance{false};
  std::thread maintenance_thread([&service, &stop_maintenance, maintenance_interval_sec]() {
    while (!stop_maintenance.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(maintenance_interval_sec));
      if (stop_maintenance.load()) break;
      service.RunMaintenanceOnce();
    }
  });

  grpc::ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  builder.SetMaxReceiveMessageSize(64 * 1024 * 1024);  // allow reasonably large bulk payloads
  builder.SetMaxSendMessageSize(64 * 1024 * 1024);

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (!server) {
    std::cerr << "dse_engine: failed to start gRPC server on " << server_address << std::endl;
    return 1;
  }

  std::cout << "dse_engine[" << node_id << "] listening on " << server_address
            << " (data_dir=" << data_dir << ", maintenance_interval="
            << maintenance_interval_sec << "s)" << std::endl;
  server->Wait();

  stop_maintenance.store(true);
  maintenance_thread.join();
  return 0;
}

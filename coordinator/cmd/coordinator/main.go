// dse-coordinator is the Go coordinator-node binary: it exposes the REST
// API, participates in the Raft-replicated cluster-metadata group, and
// scatter-gathers queries out to C++ engine nodes over gRPC.
//
// Usage (single-node "cluster" — the simplest way to try the whole system):
//
//	dse-coordinator -bootstrap -node-id=coord1 -http-addr=:8080 \
//	    -raft-addr=127.0.0.1:9000 -raft-dir=./data/coord1/raft \
//	    -engine-nodes=localhost:7001,localhost:7002
//
// Usage (multi-node cluster): start the first node with -bootstrap, then
// start additional nodes WITHOUT -bootstrap and call
// POST /_cluster/join {"node_id":"...","raft_addr":"..."} against the
// leader's HTTP API to admit them (see scripts/run_cluster.sh).
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"log/slog"
	"net/http"
	"os"
	"time"

	"dse/coordinator/internal/api"
	"dse/coordinator/internal/cluster"
	"dse/coordinator/internal/config"
	"dse/coordinator/internal/engineclient"
	"dse/coordinator/internal/routing"
)

type joinRequest struct {
	NodeID   string `json:"node_id"`
	RaftAddr string `json:"raft_addr"`
}

func main() {
	slog.SetDefault(slog.New(slog.NewJSONHandler(os.Stdout, nil)))

	cfg := config.Load()
	slog.Info("starting coordinator", "node_id", cfg.NodeID, "http_addr", cfg.HTTPAddr,
		"raft_addr", cfg.RaftAddr, "bootstrap", cfg.Bootstrap, "engine_nodes", cfg.EngineNodes)

	node, err := cluster.NewNode(cluster.Config{
		NodeID:    cfg.NodeID,
		RaftAddr:  cfg.RaftAddr,
		RaftDir:   cfg.RaftDir,
		Bootstrap: cfg.Bootstrap,
	})
	if err != nil {
		slog.Error("failed to start raft node", "err", err)
		os.Exit(1)
	}

	pool := engineclient.NewPool()

	// If this node bootstrapped a fresh cluster, register the engine nodes
	// it was told about at startup so CreateIndex has somewhere to place
	// shards. On a node joining an EXISTING cluster, engine-node membership
	// is already replicated via raft and this is a harmless idempotent
	// no-op registration attempt (skipped entirely if not leader).
	if cfg.Bootstrap {
		go registerInitialEngineNodes(node, cfg.EngineNodes)
	}

	if cfg.JoinAddr != "" {
		go requestJoin(cfg.JoinAddr, cfg.NodeID, cfg.RaftAddr)
	}

	router := routing.NewRouter(node, pool)
	mux := api.NewRouter(router)

	// /_cluster/join: an existing (ideally leader) node uses this to admit a
	// new node into the raft cluster. Not part of the api package's
	// Coordinator-interface-driven routes since it's raft-specific
	// cluster-membership plumbing, not a search-engine data-plane concern.
	mux.Post("/_cluster/join", func(w http.ResponseWriter, r *http.Request) {
		var req joinRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			http.Error(w, `{"error":"invalid json"}`, http.StatusBadRequest)
			return
		}
		if err := node.Join(req.NodeID, req.RaftAddr); err != nil {
			http.Error(w, `{"error":"`+err.Error()+`"}`, http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"joined":true}`))
	})

	mux.Get("/health", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"status":"ok"}`))
	})

	slog.Info("coordinator HTTP API listening", "addr", cfg.HTTPAddr)
	if err := http.ListenAndServe(cfg.HTTPAddr, mux); err != nil {
		slog.Error("http server exited", "err", err)
		os.Exit(1)
	}
}

// registerInitialEngineNodes waits (briefly, since leadership is granted
// almost immediately after a single-node bootstrap) for this node to become
// leader, then commits a RegisterEngineNode command for each configured
// engine node address.
func registerInitialEngineNodes(node *cluster.Node, addrs []string) {
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if node.IsLeader() {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	if !node.IsLeader() {
		slog.Warn("did not become leader in time; skipping initial engine node registration")
		return
	}
	for _, addr := range addrs {
		cmd := cluster.Command{Type: cluster.CmdRegisterEngineNode, RegisterEngineNode: &cluster.RegisterEngineNodeCmd{Addr: addr}}
		if _, err := node.Apply(cmd, 5*time.Second); err != nil {
			slog.Error("failed to register engine node", "addr", addr, "err", err)
			continue
		}
		slog.Info("registered engine node", "addr", addr)
	}
}

// requestJoin asks an existing cluster member (joinAddr, an HTTP address) to
// admit this node as a raft voter.
func requestJoin(joinAddr, nodeID, raftAddr string) {
	time.Sleep(500 * time.Millisecond) // let this node's own raft transport start listening first
	body, _ := json.Marshal(joinRequest{NodeID: nodeID, RaftAddr: raftAddr})
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, "http://"+joinAddr+"/_cluster/join",
		bytes.NewReader(body))
	if err != nil {
		slog.Error("failed to build join request", "err", err)
		return
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		slog.Error("failed to send join request", "join_addr", joinAddr, "err", err)
		return
	}
	defer resp.Body.Close()
	slog.Info("join request sent", "join_addr", joinAddr, "status", resp.StatusCode)
}

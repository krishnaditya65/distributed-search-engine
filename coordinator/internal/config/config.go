// Package config provides command-line-flag-driven configuration for a
// single coordinator node process.
//
// In the broader distributed search engine, a "coordinator" node is one
// process that simultaneously:
//   - serves the REST API that clients and operators talk to;
//   - participates as a member of a Raft consensus cluster alongside other
//     coordinator nodes, so that cluster metadata (index definitions, shard
//     assignments, node membership, etc.) stays consistent and survives
//     individual node failures; and
//   - is aware of a set of engine (C++ data node) gRPC addresses — the
//     tier that actually stores and searches shard data.
//
// A production deployment typically runs 3 or more coordinator nodes
// joined into a single Raft cluster (for quorum/fault-tolerance), each of
// which knows about the same pool of engine nodes. This package is only
// concerned with parsing the flags that describe one such node's identity,
// network addresses, Raft bootstrap/join behavior, and default
// index-creation parameters; it does not start any servers itself.
package config

import (
	"flag"
	"os"
	"strings"

	"github.com/google/uuid"
)

// NodeConfig holds all configuration needed to start a single coordinator
// node: its identity, the addresses it binds/advertises for the REST API
// and Raft transport, where it persists Raft state, how it joins (or
// bootstraps) the Raft cluster, the initial set of known engine (data
// node) gRPC addresses, and default parameters for newly created indices.
type NodeConfig struct {
	// NodeID uniquely identifies this coordinator node within the Raft
	// cluster, e.g. "node1".
	NodeID string

	// HTTPAddr is the bind address for the REST API server, e.g. ":8080".
	HTTPAddr string

	// RaftAddr is the bind/advertise address for the Raft transport, e.g.
	// "127.0.0.1:9000". It must be a real, dialable host:port (not just
	// ":9000"), since raft's TCPTransport advertises this address to other
	// cluster members so they can connect to it.
	RaftAddr string

	// RaftDir is the directory used for Raft's log store, stable store,
	// and snapshot store.
	RaftDir string

	// Bootstrap, if true, tells this node to bootstrap a brand-new
	// single-node Raft cluster on startup.
	Bootstrap bool

	// JoinAddr, if non-empty, is the HTTP address (host:port, no scheme)
	// of an existing cluster member that this node should request to join
	// through. In practice this is mutually exclusive with Bootstrap, but
	// that is not enforced here.
	JoinAddr string

	// EngineNodes is the initial, statically-known list of engine (C++
	// data node) gRPC addresses, parsed from a comma-separated flag value.
	// Dynamic engine-node registration via Raft is handled elsewhere.
	EngineNodes []string

	// NumShards is the default number of shards used for newly created
	// indices.
	NumShards int

	// ReplicationFactor is the default replication factor used for newly
	// created indices.
	ReplicationFactor int
}

// Load parses os.Args[1:] using a fresh *flag.FlagSet (never the global
// flag.CommandLine) and returns a populated NodeConfig. Using a fresh
// FlagSet per call means Load may safely be called more than once within
// the same process (e.g. from multiple tests) without panicking due to
// flags being redefined on the global flag set.
//
// Flags:
//
//	-node-id       unique id for this coordinator node (default: generated,
//	               "node-" + short random suffix)
//	-http-addr     REST API bind address (default ":8080")
//	-raft-addr     raft transport bind address (default "127.0.0.1:9000")
//	-raft-dir      directory for raft's log/stable/snapshot stores (default "./data/raft")
//	-bootstrap     bootstrap a brand-new single-node raft cluster (default false)
//	-join          HTTP address of an existing cluster member to join through (default "")
//	-engine-nodes  comma-separated list of initial engine gRPC addresses (default "")
//	-shards        default number of shards for new indices (default 6)
//	-replicas      default replication factor for new indices (default 2)
func Load() NodeConfig {
	flagSet := flag.NewFlagSet(os.Args[0], flag.ExitOnError)

	nodeID := flagSet.String("node-id", "", "unique id for this coordinator node")
	httpAddr := flagSet.String("http-addr", ":8080", "REST API bind address")
	raftAddr := flagSet.String("raft-addr", "127.0.0.1:9000", "raft transport bind address (must be a real host:port)")
	raftDir := flagSet.String("raft-dir", "./data/raft", "directory for raft's log/stable/snapshot stores")
	bootstrap := flagSet.Bool("bootstrap", false, "bootstrap a brand-new single-node raft cluster")
	join := flagSet.String("join", "", "HTTP address of an existing cluster member to join through")
	engineNodes := flagSet.String("engine-nodes", "", "comma-separated list of initial engine node gRPC addresses")
	shards := flagSet.Int("shards", 6, "default number of shards for newly created indices")
	replicas := flagSet.Int("replicas", 2, "default replication factor for newly created indices")

	_ = flagSet.Parse(os.Args[1:])

	id := strings.TrimSpace(*nodeID)
	if id == "" {
		id = "node-" + uuid.NewString()[:8]
	}

	return NodeConfig{
		NodeID:            id,
		HTTPAddr:          strings.TrimSpace(*httpAddr),
		RaftAddr:          strings.TrimSpace(*raftAddr),
		RaftDir:           strings.TrimSpace(*raftDir),
		Bootstrap:         *bootstrap,
		JoinAddr:          strings.TrimSpace(*join),
		EngineNodes:       parseEngineNodes(*engineNodes),
		NumShards:         *shards,
		ReplicationFactor: *replicas,
	}
}

// parseEngineNodes splits a comma-separated list of engine node addresses,
// trims whitespace from each entry, and drops any resulting empty strings
// (e.g. from trailing commas or an entirely empty input).
func parseEngineNodes(raw string) []string {
	if strings.TrimSpace(raw) == "" {
		return nil
	}

	parts := strings.Split(raw, ",")
	out := make([]string, 0, len(parts))
	for _, part := range parts {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		out = append(out, part)
	}
	return out
}

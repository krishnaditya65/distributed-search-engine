package cluster

import (
	"encoding/json"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"time"

	"github.com/hashicorp/raft"
	raftboltdb "github.com/hashicorp/raft-boltdb/v2"
)

// Node wraps a single coordinator's Raft participation: the *raft.Raft
// instance itself, the replicated FSM, and the bookkeeping needed to
// bootstrap a brand-new cluster or join an existing one.
type Node struct {
	NodeID string
	Raft   *raft.Raft
	FSM    *FSM
}

// Config is the minimal set of knobs Node needs (deliberately not importing
// internal/config here, to keep `cluster` independent of the CLI-flags
// layer — main.go adapts config.NodeConfig into this).
type Config struct {
	NodeID    string
	RaftAddr  string // host:port, must be dialable by other nodes
	RaftDir   string
	Bootstrap bool
}

// NewNode constructs and starts a raft.Raft instance backed by an on-disk
// BoltDB log/stable store and a file-based snapshot store, all rooted at
// cfg.RaftDir. If cfg.Bootstrap is true, this node immediately forms a new
// single-node cluster (which can later grow via Join); otherwise it starts
// as a raft participant with no known peers, expecting either to already be
// listed in an existing cluster's configuration (restart case) or to be
// added via another node's Join call (fresh node joining an existing
// cluster case).
func NewNode(cfg Config) (*Node, error) {
	if err := os.MkdirAll(cfg.RaftDir, 0o755); err != nil {
		return nil, fmt.Errorf("create raft dir: %w", err)
	}

	fsm := NewFSM()

	raftConfig := raft.DefaultConfig()
	raftConfig.LocalID = raft.ServerID(cfg.NodeID)

	addr, err := net.ResolveTCPAddr("tcp", cfg.RaftAddr)
	if err != nil {
		return nil, fmt.Errorf("resolve raft addr %q: %w", cfg.RaftAddr, err)
	}
	transport, err := raft.NewTCPTransport(cfg.RaftAddr, addr, 3, 10*time.Second, os.Stderr)
	if err != nil {
		return nil, fmt.Errorf("create raft transport: %w", err)
	}

	snapshots, err := raft.NewFileSnapshotStore(cfg.RaftDir, 2, os.Stderr)
	if err != nil {
		return nil, fmt.Errorf("create snapshot store: %w", err)
	}

	boltStore, err := raftboltdb.New(raftboltdb.Options{
		Path: filepath.Join(cfg.RaftDir, "raft.db"),
	})
	if err != nil {
		return nil, fmt.Errorf("create bolt store: %w", err)
	}

	raftInst, err := raft.NewRaft(raftConfig, fsm, boltStore, boltStore, snapshots, transport)
	if err != nil {
		return nil, fmt.Errorf("create raft instance: %w", err)
	}

	if cfg.Bootstrap {
		hasState, err := raft.HasExistingState(boltStore, boltStore, snapshots)
		if err != nil {
			return nil, fmt.Errorf("check existing raft state: %w", err)
		}
		if !hasState {
			bootstrapConfig := raft.Configuration{
				Servers: []raft.Server{
					{ID: raftConfig.LocalID, Address: transport.LocalAddr()},
				},
			}
			if f := raftInst.BootstrapCluster(bootstrapConfig); f.Error() != nil {
				return nil, fmt.Errorf("bootstrap cluster: %w", f.Error())
			}
		}
	}

	return &Node{NodeID: cfg.NodeID, Raft: raftInst, FSM: fsm}, nil
}

// IsLeader reports whether this node currently believes it is the raft
// leader. Like all raft leadership checks, this can be stale the instant
// after it's read (a lease-based `LeaderWithID`/`VerifyLeader` pattern would
// be needed for linearizable reads — not implemented here, documented as a
// simplification appropriate for this system's read-your-writes-per-request
// consistency goals rather than strict linearizability).
func (n *Node) IsLeader() bool {
	return n.Raft.State() == raft.Leader
}

// LeaderAddr returns the raft transport address of the current leader (as
// last observed by this node), or "" if unknown.
func (n *Node) LeaderAddr() string {
	addr, _ := n.Raft.LeaderWithID()
	return string(addr)
}

// Apply proposes `cmd` to the raft log and blocks until it is committed (or
// times out / fails). Must only be called on the leader; raft.Apply returns
// raft.ErrNotLeader (surfaced via the ApplyFuture's Error()) otherwise.
func (n *Node) Apply(cmd Command, timeout time.Duration) (ApplyResult, error) {
	b, err := json.Marshal(cmd)
	if err != nil {
		return ApplyResult{}, fmt.Errorf("marshal command: %w", err)
	}
	future := n.Raft.Apply(b, timeout)
	if err := future.Error(); err != nil {
		return ApplyResult{}, err
	}
	resp, ok := future.Response().(ApplyResult)
	if !ok {
		return ApplyResult{}, fmt.Errorf("unexpected apply response type %T", future.Response())
	}
	if resp.Error != "" {
		return resp, fmt.Errorf("%s", resp.Error)
	}
	return resp, nil
}

// Join admits a new node into the cluster as a voter. Must be called on the
// current leader (typically via an HTTP "/cluster/join" request forwarded to
// whichever node believes itself to be leader).
func (n *Node) Join(nodeID, raftAddr string) error {
	if !n.IsLeader() {
		return fmt.Errorf("not the leader; current leader is %q", n.LeaderAddr())
	}
	future := n.Raft.AddVoter(raft.ServerID(nodeID), raft.ServerAddress(raftAddr), 0, 10*time.Second)
	return future.Error()
}

// Peers returns the raft-transport addresses of every server currently in
// the cluster configuration, for cluster-status reporting.
func (n *Node) Peers() ([]string, error) {
	confFuture := n.Raft.GetConfiguration()
	if err := confFuture.Error(); err != nil {
		return nil, err
	}
	var out []string
	for _, srv := range confFuture.Configuration().Servers {
		out = append(out, string(srv.Address))
	}
	return out, nil
}

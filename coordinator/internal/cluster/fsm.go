package cluster

import (
	"encoding/json"
	"fmt"
	"io"
	"sync"

	"github.com/hashicorp/raft"
)

// CommandType tags which kind of mutation a raft log entry represents.
type CommandType string

const (
	CmdCreateIndex         CommandType = "create_index"
	CmdRegisterEngineNode  CommandType = "register_engine_node"
)

// Command is the single envelope type appended to the raft log; exactly one
// of the typed payload fields is populated depending on Type. JSON-encoding
// the whole command (rather than raw structs) keeps the log format simple,
// human-inspectable, and trivially versioned by adding new optional fields.
type Command struct {
	Type               CommandType             `json:"type"`
	CreateIndex        *CreateIndexCommand      `json:"create_index,omitempty"`
	RegisterEngineNode *RegisterEngineNodeCmd   `json:"register_engine_node,omitempty"`
}

// CreateIndexCommand captures everything needed to deterministically compute
// shard placement: the engine node pool is captured AT COMMAND-BUILD TIME
// (by the raft leader, in routing.Router.CreateIndex) so that every
// coordinator replaying this log entry computes the exact same placement,
// regardless of what its own locally-known EngineNodes list looks like at
// apply time (placement must be a pure function of the command's own data,
// not of mutable external state, for raft's replicated-state-machine
// determinism guarantee to hold).
type CreateIndexCommand struct {
	Name        string         `json:"name"`
	Mappings    []FieldMapping `json:"mappings"`
	NumShards   int            `json:"num_shards"`
	NumReplicas int            `json:"num_replicas"`
	EngineNodes []string       `json:"engine_nodes"`
}

type RegisterEngineNodeCmd struct {
	Addr string `json:"addr"`
}

// ApplyResult is what FSM.Apply returns (retrieved by the caller via
// raft.ApplyFuture.Response()). A non-empty Error means the command was
// rejected (e.g. duplicate index name) — note this is still applied/logged
// (raft doesn't support "validate before commit" natively in this simple
// usage), it just has no effect beyond recording that the attempt happened.
type ApplyResult struct {
	Error string      `json:"error,omitempty"`
	Value interface{} `json:"value,omitempty"`
}

// FSM implements raft.FSM. All access to `state` is guarded by mu so
// concurrent reads (State()) never race with Apply() (which raft always
// calls from a single goroutine, but State() is called from arbitrary HTTP
// handler goroutines).
type FSM struct {
	mu    sync.RWMutex
	state *State
}

func NewFSM() *FSM {
	return &FSM{state: newEmptyState()}
}

// State returns a deep-copied snapshot of the current cluster state, safe
// for the caller to read without further synchronization.
func (f *FSM) State() *State {
	f.mu.RLock()
	defer f.mu.RUnlock()
	return f.state.clone()
}

// Apply implements raft.FSM. Invoked once per committed log entry, in log
// order, on every node (including the leader) — this is what makes the
// state machine "replicated": every node ends up in the same state by
// deterministically replaying the same command sequence.
func (f *FSM) Apply(entry *raft.Log) interface{} {
	var cmd Command
	if err := json.Unmarshal(entry.Data, &cmd); err != nil {
		return ApplyResult{Error: fmt.Sprintf("malformed command: %v", err)}
	}

	f.mu.Lock()
	defer f.mu.Unlock()

	switch cmd.Type {
	case CmdCreateIndex:
		return f.applyCreateIndex(cmd.CreateIndex)
	case CmdRegisterEngineNode:
		return f.applyRegisterEngineNode(cmd.RegisterEngineNode)
	default:
		return ApplyResult{Error: fmt.Sprintf("unknown command type: %s", cmd.Type)}
	}
}

func (f *FSM) applyCreateIndex(cmd *CreateIndexCommand) ApplyResult {
	if cmd == nil {
		return ApplyResult{Error: "create_index command missing payload"}
	}
	if _, exists := f.state.Indices[cmd.Name]; exists {
		return ApplyResult{Error: fmt.Sprintf("index %q already exists", cmd.Name)}
	}
	if len(cmd.EngineNodes) == 0 {
		return ApplyResult{Error: "no engine nodes available to place shards on"}
	}
	numShards := cmd.NumShards
	if numShards <= 0 {
		numShards = 1
	}
	numReplicas := cmd.NumReplicas
	if numReplicas < 0 {
		numReplicas = 0
	}
	// Cannot have more replica copies than there are OTHER distinct engine
	// nodes to place them on.
	maxReplicas := len(cmd.EngineNodes) - 1
	if maxReplicas < 0 {
		maxReplicas = 0
	}
	if numReplicas > maxReplicas {
		numReplicas = maxReplicas
	}

	shards := make([]ShardMeta, 0, numShards)
	for i := 0; i < numShards; i++ {
		primary := cmd.EngineNodes[i%len(cmd.EngineNodes)]
		var replicas []string
		for j := 0; j < numReplicas; j++ {
			candidate := cmd.EngineNodes[(i+j+1)%len(cmd.EngineNodes)]
			if candidate == primary || containsString(replicas, candidate) {
				continue
			}
			replicas = append(replicas, candidate)
		}
		shards = append(shards, ShardMeta{
			ShardID:  fmt.Sprintf("%s-shard-%d", cmd.Name, i),
			Index:    cmd.Name,
			Primary:  primary,
			Replicas: replicas,
		})
	}

	f.state.Indices[cmd.Name] = &IndexMeta{
		Name:     cmd.Name,
		Mappings: append([]FieldMapping(nil), cmd.Mappings...),
		Shards:   shards,
	}
	return ApplyResult{Value: f.state.Indices[cmd.Name]}
}

func (f *FSM) applyRegisterEngineNode(cmd *RegisterEngineNodeCmd) ApplyResult {
	if cmd == nil || cmd.Addr == "" {
		return ApplyResult{Error: "register_engine_node command missing addr"}
	}
	if !containsString(f.state.EngineNodes, cmd.Addr) {
		f.state.EngineNodes = append(f.state.EngineNodes, cmd.Addr)
	}
	return ApplyResult{Value: f.state.EngineNodes}
}

// --- raft.FSM snapshot/restore -----------------------------------------

type fsmSnapshot struct {
	state *State
}

func (f *FSM) Snapshot() (raft.FSMSnapshot, error) {
	f.mu.RLock()
	defer f.mu.RUnlock()
	return &fsmSnapshot{state: f.state.clone()}, nil
}

func (s *fsmSnapshot) Persist(sink raft.SnapshotSink) error {
	b, err := json.Marshal(s.state)
	if err != nil {
		sink.Cancel()
		return err
	}
	if _, err := sink.Write(b); err != nil {
		sink.Cancel()
		return err
	}
	return sink.Close()
}

func (s *fsmSnapshot) Release() {}

func (f *FSM) Restore(rc io.ReadCloser) error {
	defer rc.Close()
	var state State
	if err := json.NewDecoder(rc).Decode(&state); err != nil {
		return err
	}
	if state.Indices == nil {
		state.Indices = make(map[string]*IndexMeta)
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	f.state = &state
	return nil
}

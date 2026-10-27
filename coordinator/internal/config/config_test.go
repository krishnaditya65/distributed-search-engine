package config

import (
	"os"
	"reflect"
	"testing"
)

// withArgs sets os.Args for the duration of the given function and restores
// the original os.Args afterward.
func withArgs(t *testing.T, args []string, fn func()) {
	t.Helper()
	orig := os.Args
	defer func() { os.Args = orig }()
	os.Args = args
	fn()
}

func TestLoad_ParsesFlags(t *testing.T) {
	withArgs(t, []string{
		"cmd",
		"-node-id=test1",
		"-http-addr=:9090",
		"-raft-addr=127.0.0.1:9100",
		"-raft-dir=/tmp/raft-data",
		"-bootstrap=true",
		"-join=127.0.0.1:8080",
		"-engine-nodes=a:1, b:2 ,c:3",
		"-shards=3",
		"-replicas=1",
	}, func() {
		cfg := Load()

		if cfg.NodeID != "test1" {
			t.Errorf("NodeID = %q, want %q", cfg.NodeID, "test1")
		}
		if cfg.HTTPAddr != ":9090" {
			t.Errorf("HTTPAddr = %q, want %q", cfg.HTTPAddr, ":9090")
		}
		if cfg.RaftAddr != "127.0.0.1:9100" {
			t.Errorf("RaftAddr = %q, want %q", cfg.RaftAddr, "127.0.0.1:9100")
		}
		if cfg.RaftDir != "/tmp/raft-data" {
			t.Errorf("RaftDir = %q, want %q", cfg.RaftDir, "/tmp/raft-data")
		}
		if !cfg.Bootstrap {
			t.Errorf("Bootstrap = %v, want true", cfg.Bootstrap)
		}
		if cfg.JoinAddr != "127.0.0.1:8080" {
			t.Errorf("JoinAddr = %q, want %q", cfg.JoinAddr, "127.0.0.1:8080")
		}
		want := []string{"a:1", "b:2", "c:3"}
		if !reflect.DeepEqual(cfg.EngineNodes, want) {
			t.Errorf("EngineNodes = %#v, want %#v", cfg.EngineNodes, want)
		}
		if cfg.NumShards != 3 {
			t.Errorf("NumShards = %d, want 3", cfg.NumShards)
		}
		if cfg.ReplicationFactor != 1 {
			t.Errorf("ReplicationFactor = %d, want 1", cfg.ReplicationFactor)
		}
	})
}

func TestLoad_CalledTwice_NoPanic(t *testing.T) {
	withArgs(t, []string{"cmd", "-node-id=first"}, func() {
		firstCfg := Load()
		if firstCfg.NodeID != "first" {
			t.Errorf("NodeID = %q, want %q", firstCfg.NodeID, "first")
		}
	})

	withArgs(t, []string{"cmd", "-node-id=second"}, func() {
		secondCfg := Load()
		if secondCfg.NodeID != "second" {
			t.Errorf("NodeID = %q, want %q", secondCfg.NodeID, "second")
		}
	})
}

func TestLoad_Defaults(t *testing.T) {
	withArgs(t, []string{"cmd"}, func() {
		cfg := Load()

		if cfg.HTTPAddr != ":8080" {
			t.Errorf("HTTPAddr = %q, want %q", cfg.HTTPAddr, ":8080")
		}
		if cfg.RaftAddr != "127.0.0.1:9000" {
			t.Errorf("RaftAddr = %q, want %q", cfg.RaftAddr, "127.0.0.1:9000")
		}
		if cfg.RaftDir != "./data/raft" {
			t.Errorf("RaftDir = %q, want %q", cfg.RaftDir, "./data/raft")
		}
		if cfg.Bootstrap {
			t.Errorf("Bootstrap = %v, want false", cfg.Bootstrap)
		}
		if cfg.JoinAddr != "" {
			t.Errorf("JoinAddr = %q, want empty", cfg.JoinAddr)
		}
		if len(cfg.EngineNodes) != 0 {
			t.Errorf("EngineNodes = %#v, want empty", cfg.EngineNodes)
		}
		if cfg.NumShards != 6 {
			t.Errorf("NumShards = %d, want 6", cfg.NumShards)
		}
		if cfg.ReplicationFactor != 2 {
			t.Errorf("ReplicationFactor = %d, want 2", cfg.ReplicationFactor)
		}
		if cfg.NodeID == "" {
			t.Errorf("NodeID should be auto-generated, got empty string")
		}
	})
}

func TestLoad_EmptyEngineNodes(t *testing.T) {
	withArgs(t, []string{"cmd", "-engine-nodes="}, func() {
		cfg := Load()
		if len(cfg.EngineNodes) != 0 {
			t.Errorf("EngineNodes = %#v, want empty slice", cfg.EngineNodes)
		}
	})
}

func TestLoad_EngineNodesTrailingComma(t *testing.T) {
	withArgs(t, []string{"cmd", "-engine-nodes=a:1,b:2,,"}, func() {
		cfg := Load()
		want := []string{"a:1", "b:2"}
		if !reflect.DeepEqual(cfg.EngineNodes, want) {
			t.Errorf("EngineNodes = %#v, want %#v", cfg.EngineNodes, want)
		}
	})
}

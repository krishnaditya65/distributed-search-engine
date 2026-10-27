// Package engineclient manages gRPC connections from the coordinator to
// individual C++ engine (data node) processes, speaking the EngineService
// contract defined in proto/engine.proto.
package engineclient

import (
	"fmt"
	"sync"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	"dse/coordinator/internal/genproto"
)

// Client is a single gRPC connection to one engine node.
type Client struct {
	Addr string
	conn *grpc.ClientConn
	api  genproto.EngineServiceClient
}

// API returns the generated gRPC client stub for issuing EngineService RPCs.
func (c *Client) API() genproto.EngineServiceClient { return c.api }

// Close releases the underlying connection.
func (c *Client) Close() error { return c.conn.Close() }

func dial(addr string) (*Client, error) {
	conn, err := grpc.NewClient(addr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return nil, fmt.Errorf("dial engine node %s: %w", addr, err)
	}
	return &Client{Addr: addr, conn: conn, api: genproto.NewEngineServiceClient(conn)}, nil
}

// Pool lazily dials and caches one Client per engine node address, so
// repeated scatter-gather calls to the same shard's primary/replicas reuse
// a single long-lived HTTP/2 connection rather than reconnecting every
// request.
type Pool struct {
	mu      sync.Mutex
	clients map[string]*Client
}

// NewPool constructs an empty connection pool.
func NewPool() *Pool {
	return &Pool{clients: make(map[string]*Client)}
}

// Get returns the cached Client for addr, dialing a new one on first use.
// The returned Client (and its underlying connection) is safe for
// concurrent use by multiple goroutines, matching grpc.ClientConn's own
// concurrency guarantees.
func (p *Pool) Get(addr string) (*Client, error) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if c, ok := p.clients[addr]; ok {
		return c, nil
	}
	c, err := dial(addr)
	if err != nil {
		return nil, err
	}
	p.clients[addr] = c
	return c, nil
}

// Close closes every connection currently held by the pool.
func (p *Pool) Close() {
	p.mu.Lock()
	defer p.mu.Unlock()
	for _, c := range p.clients {
		_ = c.Close()
	}
	p.clients = make(map[string]*Client)
}

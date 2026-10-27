package api

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"sync"
	"testing"
)

// fakeCoordinator is a minimal in-memory implementation of Coordinator
// sufficient to exercise the HTTP handlers.
type fakeCoordinator struct {
	mu       sync.Mutex
	indices  map[string]CreateIndexRequest
	docs     map[string]map[string]json.RawMessage // index -> docID -> body
	versions map[string]map[string]int64           // index -> docID -> version

	lastSearchReq SearchRequest
}

func newFakeCoordinator() *fakeCoordinator {
	return &fakeCoordinator{
		indices:  make(map[string]CreateIndexRequest),
		docs:     make(map[string]map[string]json.RawMessage),
		versions: make(map[string]map[string]int64),
	}
}

func (f *fakeCoordinator) CreateIndex(ctx context.Context, index string, req CreateIndexRequest) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.indices[index] = req
	if f.docs[index] == nil {
		f.docs[index] = make(map[string]json.RawMessage)
		f.versions[index] = make(map[string]int64)
	}
	return nil
}

func (f *fakeCoordinator) IndexDocument(ctx context.Context, index, docID string, body json.RawMessage) (int64, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.docs[index] == nil {
		f.docs[index] = make(map[string]json.RawMessage)
		f.versions[index] = make(map[string]int64)
	}
	f.versions[index][docID]++
	f.docs[index][docID] = body
	return f.versions[index][docID], nil
}

func (f *fakeCoordinator) GetDocument(ctx context.Context, index, docID string) (json.RawMessage, bool, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	byID, ok := f.docs[index]
	if !ok {
		return nil, false, nil
	}
	body, ok := byID[docID]
	if !ok {
		return nil, false, nil
	}
	return body, true, nil
}

func (f *fakeCoordinator) DeleteDocument(ctx context.Context, index, docID string) (bool, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	byID, ok := f.docs[index]
	if !ok {
		return false, nil
	}
	if _, ok := byID[docID]; !ok {
		return false, nil
	}
	delete(byID, docID)
	return true, nil
}

func (f *fakeCoordinator) BulkIndex(ctx context.Context, index string, ops []BulkOp) ([]BulkOpResult, error) {
	results := make([]BulkOpResult, 0, len(ops))
	for _, op := range ops {
		switch op.Type {
		case "index":
			_, err := f.IndexDocument(ctx, index, op.DocID, op.Body)
			if err != nil {
				results = append(results, BulkOpResult{Success: false, Error: err.Error(), DocID: op.DocID})
				continue
			}
			results = append(results, BulkOpResult{Success: true, DocID: op.DocID})
		case "delete":
			_, err := f.DeleteDocument(ctx, index, op.DocID)
			if err != nil {
				results = append(results, BulkOpResult{Success: false, Error: err.Error(), DocID: op.DocID})
				continue
			}
			results = append(results, BulkOpResult{Success: true, DocID: op.DocID})
		default:
			results = append(results, BulkOpResult{Success: false, Error: "unknown op type", DocID: op.DocID})
		}
	}
	return results, nil
}

func (f *fakeCoordinator) Search(ctx context.Context, index string, req SearchRequest) (SearchResponse, error) {
	f.mu.Lock()
	f.lastSearchReq = req
	f.mu.Unlock()
	return SearchResponse{
		Hits:       []SearchHit{},
		TotalHits:  0,
		TookMillis: 1,
	}, nil
}

func (f *fakeCoordinator) ClusterStatus(ctx context.Context) (ClusterStatusResponse, error) {
	return ClusterStatusResponse{
		NodeID:   "node-1",
		IsLeader: true,
		Leader:   "node-1",
		Nodes:    []string{"node-1"},
		Shards:   []ShardInfo{{ShardID: "shard-0", Index: "products", Primary: "node-1"}},
	}, nil
}

func doRequest(t *testing.T, router http.Handler, method, path string, body []byte) *httptest.ResponseRecorder {
	t.Helper()
	var req *http.Request
	if body != nil {
		req = httptest.NewRequest(method, path, bytes.NewReader(body))
	} else {
		req = httptest.NewRequest(method, path, nil)
	}
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()
	router.ServeHTTP(rec, req)
	return rec
}

func TestCreateIndex(t *testing.T) {
	coord := newFakeCoordinator()
	router := NewRouter(coord)

	body := []byte(`{"mappings":[{"field":"title","type":"text"}],"shards":3,"replicas":1}`)
	rec := doRequest(t, router, http.MethodPut, "/indices/products", body)

	if rec.Code != http.StatusCreated {
		t.Fatalf("expected 201, got %d: %s", rec.Code, rec.Body.String())
	}

	var resp map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &resp); err != nil {
		t.Fatalf("failed to decode response: %v", err)
	}
	if ack, _ := resp["acknowledged"].(bool); !ack {
		t.Fatalf("expected acknowledged=true, got %v", resp)
	}
}

func TestIndexDocument(t *testing.T) {
	coord := newFakeCoordinator()
	router := NewRouter(coord)

	doRequest(t, router, http.MethodPut, "/indices/products", []byte(`{}`))

	docBody := []byte(`{"name":"widget","price":9.99}`)
	rec := doRequest(t, router, http.MethodPut, "/indices/products/_doc/1", docBody)

	if rec.Code != http.StatusCreated {
		t.Fatalf("expected 201, got %d: %s", rec.Code, rec.Body.String())
	}

	var resp struct {
		DocID   string `json:"doc_id"`
		Version int64  `json:"version"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &resp); err != nil {
		t.Fatalf("failed to decode response: %v", err)
	}
	if resp.DocID != "1" {
		t.Fatalf("expected doc_id=1, got %q", resp.DocID)
	}
	if resp.Version != 1 {
		t.Fatalf("expected version=1, got %d", resp.Version)
	}
}

func TestGetDocumentFoundAndNotFound(t *testing.T) {
	coord := newFakeCoordinator()
	router := NewRouter(coord)

	doRequest(t, router, http.MethodPut, "/indices/products", []byte(`{}`))
	docBody := []byte(`{"name":"widget"}`)
	doRequest(t, router, http.MethodPut, "/indices/products/_doc/1", docBody)

	// found
	rec := doRequest(t, router, http.MethodGet, "/indices/products/_doc/1", nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", rec.Code, rec.Body.String())
	}
	var resp struct {
		DocID  string          `json:"doc_id"`
		Found  bool            `json:"found"`
		Source json.RawMessage `json:"source"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &resp); err != nil {
		t.Fatalf("failed to decode response: %v", err)
	}
	if !resp.Found {
		t.Fatalf("expected found=true")
	}
	if !bytes.Equal(bytes.TrimSpace(resp.Source), docBody) {
		t.Fatalf("expected source=%s, got %s", docBody, resp.Source)
	}

	// not found
	rec = doRequest(t, router, http.MethodGet, "/indices/products/_doc/doesnotexist", nil)
	if rec.Code != http.StatusNotFound {
		t.Fatalf("expected 404, got %d: %s", rec.Code, rec.Body.String())
	}
	var notFoundResp struct {
		DocID string `json:"doc_id"`
		Found bool   `json:"found"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &notFoundResp); err != nil {
		t.Fatalf("failed to decode response: %v", err)
	}
	if notFoundResp.Found {
		t.Fatalf("expected found=false")
	}
}

func TestDeleteDocument(t *testing.T) {
	coord := newFakeCoordinator()
	router := NewRouter(coord)

	doRequest(t, router, http.MethodPut, "/indices/products", []byte(`{}`))
	doRequest(t, router, http.MethodPut, "/indices/products/_doc/1", []byte(`{"name":"widget"}`))

	rec := doRequest(t, router, http.MethodDelete, "/indices/products/_doc/1", nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", rec.Code, rec.Body.String())
	}
	var resp struct {
		Found bool `json:"found"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &resp); err != nil {
		t.Fatalf("failed to decode response: %v", err)
	}
	if !resp.Found {
		t.Fatalf("expected found=true")
	}

	// subsequent GET should 404
	rec = doRequest(t, router, http.MethodGet, "/indices/products/_doc/1", nil)
	if rec.Code != http.StatusNotFound {
		t.Fatalf("expected 404 after delete, got %d: %s", rec.Code, rec.Body.String())
	}
}

func TestBulkIndex(t *testing.T) {
	coord := newFakeCoordinator()
	router := NewRouter(coord)

	doRequest(t, router, http.MethodPut, "/indices/products", []byte(`{}`))
	doRequest(t, router, http.MethodPut, "/indices/products/_doc/2", []byte(`{"name":"gadget"}`))

	bulkBody := []byte(`{"ops":[
		{"type":"index","doc_id":"1","body":{"name":"widget"}},
		{"type":"delete","doc_id":"2"},
		{"type":"index","doc_id":"3","body":{"name":"gizmo"}}
	]}`)
	rec := doRequest(t, router, http.MethodPost, "/indices/products/_bulk", bulkBody)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", rec.Code, rec.Body.String())
	}

	var resp bulkResponse
	if err := json.Unmarshal(rec.Body.Bytes(), &resp); err != nil {
		t.Fatalf("failed to decode response: %v", err)
	}
	if len(resp.Results) != 3 {
		t.Fatalf("expected 3 results, got %d", len(resp.Results))
	}
	for _, r := range resp.Results {
		if !r.Success {
			t.Fatalf("expected op for %s to succeed, got error %q", r.DocID, r.Error)
		}
	}
}

func TestSearchDefaultsSize(t *testing.T) {
	coord := newFakeCoordinator()
	router := NewRouter(coord)

	rec := doRequest(t, router, http.MethodPost, "/indices/products/_search", []byte(`{}`))
	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", rec.Code, rec.Body.String())
	}

	if coord.lastSearchReq.Size != 10 {
		t.Fatalf("expected Size defaulted to 10, got %d", coord.lastSearchReq.Size)
	}

	var resp SearchResponse
	if err := json.Unmarshal(rec.Body.Bytes(), &resp); err != nil {
		t.Fatalf("failed to decode response: %v", err)
	}
}

func TestClusterHealth(t *testing.T) {
	coord := newFakeCoordinator()
	router := NewRouter(coord)

	rec := doRequest(t, router, http.MethodGet, "/_cluster/health", nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", rec.Code, rec.Body.String())
	}

	var resp ClusterStatusResponse
	if err := json.Unmarshal(rec.Body.Bytes(), &resp); err != nil {
		t.Fatalf("failed to decode response: %v", err)
	}
	if resp.NodeID != "node-1" {
		t.Fatalf("expected node_id=node-1, got %q", resp.NodeID)
	}
	if !resp.IsLeader {
		t.Fatalf("expected is_leader=true")
	}
	if len(resp.Shards) != 1 {
		t.Fatalf("expected 1 shard, got %d", len(resp.Shards))
	}
}

func TestCreateIndexMalformedJSON(t *testing.T) {
	coord := newFakeCoordinator()
	router := NewRouter(coord)

	rec := doRequest(t, router, http.MethodPut, "/indices/products", []byte(`{not valid json`))
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("expected 400, got %d: %s", rec.Code, rec.Body.String())
	}

	var resp errorResponse
	if err := json.Unmarshal(rec.Body.Bytes(), &resp); err != nil {
		t.Fatalf("failed to decode response: %v", err)
	}
	if resp.Error == "" {
		t.Fatalf("expected non-empty error message")
	}
}

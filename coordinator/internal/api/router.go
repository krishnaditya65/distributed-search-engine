package api

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"

	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"
)

// ErrIndexNotFound is returned by Coordinator methods when the referenced
// index does not exist. Handlers map it to a 404 response via errors.Is.
var ErrIndexNotFound = errors.New("api: index not found")

// Coordinator is implemented by the routing package (elsewhere in this
// repo, not by the api package) and is the sole dependency of the HTTP
// layer. All methods receive the request-scoped context.Context from the
// incoming *http.Request.
type Coordinator interface {
	CreateIndex(ctx context.Context, index string, req CreateIndexRequest) error
	IndexDocument(ctx context.Context, index, docID string, body json.RawMessage) (version int64, err error)
	GetDocument(ctx context.Context, index, docID string) (body json.RawMessage, found bool, err error)
	DeleteDocument(ctx context.Context, index, docID string) (found bool, err error)
	BulkIndex(ctx context.Context, index string, ops []BulkOp) ([]BulkOpResult, error)
	Search(ctx context.Context, index string, req SearchRequest) (SearchResponse, error)
	ClusterStatus(ctx context.Context) (ClusterStatusResponse, error)
}

// NewRouter builds the full chi.Mux for the REST API, wired against coord.
func NewRouter(coord Coordinator) *chi.Mux {
	r := chi.NewRouter()
	r.Use(middleware.Recoverer)

	h := &handlers{coord: coord}

	r.Put("/indices/{index}", h.createIndex)
	r.Put("/indices/{index}/_doc/{id}", h.indexDocument)
	r.Get("/indices/{index}/_doc/{id}", h.getDocument)
	r.Delete("/indices/{index}/_doc/{id}", h.deleteDocument)
	r.Post("/indices/{index}/_bulk", h.bulkIndex)
	r.Post("/indices/{index}/_search", h.search)
	r.Get("/_cluster/health", h.clusterHealth)

	return r
}

// handlers holds the shared Coordinator dependency for all HTTP handlers.
type handlers struct {
	coord Coordinator
}

// errorResponse is the JSON shape returned for all error responses.
type errorResponse struct {
	Error string `json:"error"`
}

func writeJSON(w http.ResponseWriter, status int, body any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if body == nil {
		return
	}
	_ = json.NewEncoder(w).Encode(body)
}

func writeError(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, errorResponse{Error: msg})
}

// pathParam fetches a required chi URL parameter, returning ok=false if it
// is missing or empty.
func pathParam(r *http.Request, name string) (string, bool) {
	v := chi.URLParam(r, name)
	if v == "" {
		return "", false
	}
	return v, true
}

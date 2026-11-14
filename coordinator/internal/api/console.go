package api

import (
	"bytes"
	"crypto/subtle"
	"errors"
	"io"
	"net/http"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"

	"dse/coordinator/ui"
)

// Console adds the operator-facing endpoints, request metrics, optional bearer-token auth
// and the embedded web UI on top of the data-plane router built by NewRouter.
type Console struct {
	inspector Inspector
	token     string
	metrics   *searchMetrics
}

func NewConsole(inspector Inspector, apiToken string, slowQueryThreshold time.Duration) *Console {
	return &Console{inspector: inspector, token: apiToken, metrics: newSearchMetrics(slowQueryThreshold)}
}

// Mount registers console endpoints and the UI on the given router.
func (c *Console) Mount(router chi.Router) {
	router.Get("/_cat/indices", c.listIndices)
	router.Get("/indices/{index}", c.describeIndex)
	router.Post("/indices/{index}/_maintenance/{action}", c.maintainIndex)
	router.Get("/_cluster/nodes", c.nodes)
	router.Get("/_console/stats", func(w http.ResponseWriter, r *http.Request) { writeJSON(w, http.StatusOK, c.metrics.snapshot()) })
	router.Get("/_console/slow-queries", func(w http.ResponseWriter, r *http.Request) { writeJSON(w, http.StatusOK, c.metrics.slowQueries()) })
	router.Handle("/ui/*", http.StripPrefix("/ui/", http.FileServer(http.FS(ui.Files))))
	router.Get("/ui", func(w http.ResponseWriter, r *http.Request) { http.Redirect(w, r, "/ui/", http.StatusFound) })
	router.Get("/", func(w http.ResponseWriter, r *http.Request) { http.Redirect(w, r, "/ui/", http.StatusFound) })
}

// Wrap applies bearer-token auth (when configured) and search metrics around next.
func (c *Console) Wrap(next http.Handler) http.Handler {
	return c.requireToken(c.measureSearches(next))
}

func isPublicPath(path string) bool {
	return path == "/health" || path == "/" || path == "/ui" || strings.HasPrefix(path, "/ui/")
}

func (c *Console) requireToken(next http.Handler) http.Handler {
	if c.token == "" {
		return next
	}

	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		presented := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")

		if !isPublicPath(r.URL.Path) && r.Method != http.MethodOptions && subtle.ConstantTimeCompare([]byte(presented), []byte(c.token)) != 1 {
			w.Header().Set("WWW-Authenticate", `Bearer realm="seekr"`)
			writeError(w, http.StatusUnauthorized, "unauthorized")
			return
		}

		next.ServeHTTP(w, r)
	})
}

type statusRecorder struct {
	http.ResponseWriter
	status int
}

func (s *statusRecorder) WriteHeader(code int) {
	s.status = code
	s.ResponseWriter.WriteHeader(code)
}

// searchIndexFromPath returns the index for POST /indices/{index}/_search, or "" for any other request.
func searchIndexFromPath(r *http.Request) string {
	parts := strings.Split(strings.Trim(r.URL.Path, "/"), "/")
	if r.Method == http.MethodPost && len(parts) == 3 && parts[0] == "indices" && parts[2] == "_search" {
		return parts[1]
	}

	return ""
}

func (c *Console) measureSearches(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		index := searchIndexFromPath(r)
		if index == "" {
			next.ServeHTTP(w, r)
			return
		}

		body, _ := io.ReadAll(io.LimitReader(r.Body, maxSearchBodyBytes))
		r.Body = io.NopCloser(io.MultiReader(bytes.NewReader(body), r.Body))

		recorder := &statusRecorder{ResponseWriter: w, status: http.StatusOK}
		started := time.Now()
		next.ServeHTTP(recorder, r)

		c.metrics.record(index, time.Since(started), recorder.status, truncate(string(body), slowQueryBodyLimit))
	})
}

const maxSearchBodyBytes = 1 << 20

func truncate(text string, limit int) string {
	if len(text) <= limit {
		return text
	}

	return text[:limit] + "…"
}

func (c *Console) listIndices(w http.ResponseWriter, r *http.Request) {
	indices, err := c.inspector.ListIndices(r.Context())
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}

	writeJSON(w, http.StatusOK, indices)
}

func (c *Console) describeIndex(w http.ResponseWriter, r *http.Request) {
	detail, err := c.inspector.DescribeIndex(r.Context(), chi.URLParam(r, "index"))
	c.respond(w, detail, err)
}

func (c *Console) maintainIndex(w http.ResponseWriter, r *http.Request) {
	action := chi.URLParam(r, "action")
	if action != "flush" && action != "merge" {
		writeError(w, http.StatusBadRequest, "action must be flush or merge")
		return
	}

	result, err := c.inspector.MaintainIndex(r.Context(), chi.URLParam(r, "index"), action)
	c.respond(w, result, err)
}

func (c *Console) nodes(w http.ResponseWriter, r *http.Request) {
	nodes, err := c.inspector.Nodes(r.Context())
	c.respond(w, nodes, err)
}

func (c *Console) respond(w http.ResponseWriter, body any, err error) {
	switch {
	case errors.Is(err, ErrIndexNotFound):
		writeError(w, http.StatusNotFound, err.Error())
	case err != nil:
		writeError(w, http.StatusInternalServerError, err.Error())
	default:
		writeJSON(w, http.StatusOK, body)
	}
}

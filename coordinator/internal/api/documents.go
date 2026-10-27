package api

import (
	"encoding/json"
	"errors"
	"io"
	"net/http"
)

// createIndex handles PUT /indices/{index}.
func (h *handlers) createIndex(w http.ResponseWriter, r *http.Request) {
	index, ok := pathParam(r, "index")
	if !ok {
		writeError(w, http.StatusBadRequest, "missing index name")
		return
	}

	var req CreateIndexRequest
	if err := decodeJSON(r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}

	if err := h.coord.CreateIndex(r.Context(), index, req); err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}

	writeJSON(w, http.StatusCreated, map[string]any{"acknowledged": true})
}

// indexDocument handles PUT /indices/{index}/_doc/{id}. It always responds
// 201 Created since Coordinator.IndexDocument does not distinguish
// create-vs-update; callers wanting that distinction can inspect the
// returned version.
func (h *handlers) indexDocument(w http.ResponseWriter, r *http.Request) {
	index, ok := pathParam(r, "index")
	if !ok {
		writeError(w, http.StatusBadRequest, "missing index name")
		return
	}
	docID, ok := pathParam(r, "id")
	if !ok {
		writeError(w, http.StatusBadRequest, "missing doc id")
		return
	}

	body, err := io.ReadAll(r.Body)
	if err != nil {
		writeError(w, http.StatusBadRequest, "failed to read request body")
		return
	}
	if !json.Valid(body) {
		writeError(w, http.StatusBadRequest, "invalid JSON document body")
		return
	}

	version, err := h.coord.IndexDocument(r.Context(), index, docID, json.RawMessage(body))
	if err != nil {
		if errors.Is(err, ErrIndexNotFound) {
			writeError(w, http.StatusNotFound, err.Error())
			return
		}
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}

	writeJSON(w, http.StatusCreated, map[string]any{
		"doc_id":  docID,
		"version": version,
	})
}

// getDocument handles GET /indices/{index}/_doc/{id}.
func (h *handlers) getDocument(w http.ResponseWriter, r *http.Request) {
	index, ok := pathParam(r, "index")
	if !ok {
		writeError(w, http.StatusBadRequest, "missing index name")
		return
	}
	docID, ok := pathParam(r, "id")
	if !ok {
		writeError(w, http.StatusBadRequest, "missing doc id")
		return
	}

	source, found, err := h.coord.GetDocument(r.Context(), index, docID)
	if err != nil {
		if errors.Is(err, ErrIndexNotFound) {
			writeError(w, http.StatusNotFound, err.Error())
			return
		}
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}

	if !found {
		writeJSON(w, http.StatusNotFound, map[string]any{
			"doc_id": docID,
			"found":  false,
		})
		return
	}

	writeJSON(w, http.StatusOK, map[string]any{
		"doc_id": docID,
		"found":  true,
		"source": source,
	})
}

// deleteDocument handles DELETE /indices/{index}/_doc/{id}.
func (h *handlers) deleteDocument(w http.ResponseWriter, r *http.Request) {
	index, ok := pathParam(r, "index")
	if !ok {
		writeError(w, http.StatusBadRequest, "missing index name")
		return
	}
	docID, ok := pathParam(r, "id")
	if !ok {
		writeError(w, http.StatusBadRequest, "missing doc id")
		return
	}

	found, err := h.coord.DeleteDocument(r.Context(), index, docID)
	if err != nil {
		if errors.Is(err, ErrIndexNotFound) {
			writeError(w, http.StatusNotFound, err.Error())
			return
		}
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}

	if !found {
		writeJSON(w, http.StatusNotFound, map[string]any{
			"doc_id": docID,
			"found":  false,
		})
		return
	}

	writeJSON(w, http.StatusOK, map[string]any{
		"doc_id": docID,
		"found":  true,
	})
}

// bulkRequest is the body of POST /indices/{index}/_bulk.
type bulkRequest struct {
	Ops []BulkOp `json:"ops"`
}

// bulkResponse is the body returned by POST /indices/{index}/_bulk.
type bulkResponse struct {
	Results []BulkOpResult `json:"results"`
}

// bulkIndex handles POST /indices/{index}/_bulk. It always responds 200
// OK; per-op success is reported in each BulkOpResult.
func (h *handlers) bulkIndex(w http.ResponseWriter, r *http.Request) {
	index, ok := pathParam(r, "index")
	if !ok {
		writeError(w, http.StatusBadRequest, "missing index name")
		return
	}

	var req bulkRequest
	if err := decodeJSON(r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}

	results, err := h.coord.BulkIndex(r.Context(), index, req.Ops)
	if err != nil {
		if errors.Is(err, ErrIndexNotFound) {
			writeError(w, http.StatusNotFound, err.Error())
			return
		}
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}

	writeJSON(w, http.StatusOK, bulkResponse{Results: results})
}

// decodeJSON decodes the request body into v, treating an empty body as a
// no-op (leaving v at its zero value) and returning a descriptive error on
// malformed JSON.
func decodeJSON(r *http.Request, v any) error {
	if r.Body == nil {
		return nil
	}
	body, err := io.ReadAll(r.Body)
	if err != nil {
		return errors.New("failed to read request body")
	}
	if len(body) == 0 {
		return nil
	}
	if err := json.Unmarshal(body, v); err != nil {
		return errors.New("invalid JSON body: " + err.Error())
	}
	return nil
}

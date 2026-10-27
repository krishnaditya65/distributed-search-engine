package api

import (
	"net/http"
)

// defaultSearchSize is applied when SearchRequest.Size is unset (<= 0).
const defaultSearchSize = 10

// search handles POST /indices/{index}/_search. An empty body ("{}" or no
// body at all) is valid and treated as a match-all query with defaults.
func (h *handlers) search(w http.ResponseWriter, r *http.Request) {
	index, ok := pathParam(r, "index")
	if !ok {
		writeError(w, http.StatusBadRequest, "missing index name")
		return
	}

	var req SearchRequest
	if err := decodeJSON(r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}

	if req.Size <= 0 {
		req.Size = defaultSearchSize
	}

	resp, err := h.coord.Search(r.Context(), index, req)
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}

	writeJSON(w, http.StatusOK, resp)
}

package api

import "net/http"

// clusterHealth handles GET /_cluster/health.
func (h *handlers) clusterHealth(w http.ResponseWriter, r *http.Request) {
	status, err := h.coord.ClusterStatus(r.Context())
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}

	writeJSON(w, http.StatusOK, status)
}

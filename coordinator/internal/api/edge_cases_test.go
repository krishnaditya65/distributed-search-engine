package api

import (
	"net/http"
	"testing"
)

func TestHandlersRejectMalformedJSONAndApplySearchDefaults(t *testing.T) {
	coord := newFakeCoordinator()
	router := NewRouter(coord)

	rec := doRequest(t, router, http.MethodPut, "/indices/products", []byte(`{"mappings":`))
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("malformed create-index JSON status = %d, want 400", rec.Code)
	}
	rec = doRequest(t, router, http.MethodPut, "/indices/products/_doc/1", []byte(`{"broken":`))
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("malformed document JSON status = %d, want 400", rec.Code)
	}

	rec = doRequest(t, router, http.MethodPost, "/indices/products/_search", []byte(`{}`))
	if rec.Code != http.StatusOK {
		t.Fatalf("empty search status = %d, want 200", rec.Code)
	}
	coord.mu.Lock()
	gotSize := coord.lastSearchReq.Size
	coord.mu.Unlock()
	if gotSize != 10 {
		t.Fatalf("default search size = %d, want 10", gotSize)
	}
}

func TestHandlersMapCoordinatorErrorsToHTTPResponses(t *testing.T) {
	coord := newFakeCoordinator()
	router := NewRouter(coord)

	if rec := doRequest(t, router, http.MethodGet, "/indices/missing/_doc/1", nil); rec.Code != http.StatusNotFound {
		t.Fatalf("missing index GET status = %d, want 404", rec.Code)
	}
	if rec := doRequest(t, router, http.MethodDelete, "/indices/missing/_doc/1", nil); rec.Code != http.StatusNotFound {
		t.Fatalf("missing index DELETE status = %d, want 404", rec.Code)
	}
}

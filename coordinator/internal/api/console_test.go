package api

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

type fakeInspector struct{}

func (fakeInspector) ListIndices(context.Context) ([]IndexSummary, error) {
	return []IndexSummary{{Name: "products", DocCount: 3, Health: "healthy"}}, nil
}
func (fakeInspector) DescribeIndex(_ context.Context, index string) (IndexDetail, error) {
	if index != "products" {
		return IndexDetail{}, ErrIndexNotFound
	}
	return IndexDetail{IndexSummary: IndexSummary{Name: index}}, nil
}
func (fakeInspector) Nodes(context.Context) (NodesResponse, error) { return NodesResponse{}, nil }
func (fakeInspector) MaintainIndex(_ context.Context, _ string, action string) (MaintenanceResult, error) {
	return MaintenanceResult{Action: action, ShardsOK: 2}, nil
}

func newConsoleHandler(token string, slow time.Duration) (http.Handler, *Console) {
	router := NewRouter(newFakeCoordinator())
	console := NewConsole(fakeInspector{}, token, slow)
	console.Mount(router)

	return console.Wrap(router), console
}

func call(handler http.Handler, method, path, body, bearer string) *httptest.ResponseRecorder {
	request := httptest.NewRequest(method, path, strings.NewReader(body))
	if bearer != "" {
		request.Header.Set("Authorization", "Bearer "+bearer)
	}

	recorder := httptest.NewRecorder()
	handler.ServeHTTP(recorder, request)

	return recorder
}

func TestTokenProtectsApiButNotUiOrHealth(t *testing.T) {
	handler, _ := newConsoleHandler("s3cret", time.Second)

	if code := call(handler, "GET", "/_cat/indices", "", "").Code; code != http.StatusUnauthorized {
		t.Fatalf("want 401 without token, got %d", code)
	}

	if code := call(handler, "GET", "/_cat/indices", "", "s3cret").Code; code != http.StatusOK {
		t.Fatalf("want 200 with token, got %d", code)
	}

	if code := call(handler, "GET", "/ui/kit.js", "", "").Code; code != http.StatusOK {
		t.Fatalf("ui must stay public, got %d", code)
	}
}

func TestSearchesAreMeasuredAndSlowOnesLogged(t *testing.T) {
	handler, console := newConsoleHandler("", 0) // threshold 0 => every search is "slow"

	call(handler, "POST", "/indices/products/_search", `{"query":"shoes"}`, "")

	stats := console.metrics.snapshot()
	if stats.SearchTotal != 1 || stats.SearchesByIndex["products"] != 1 {
		t.Fatalf("search not counted: %+v", stats)
	}

	slow := console.metrics.slowQueries()
	if len(slow) != 1 || !strings.Contains(slow[0].Query, "shoes") {
		t.Fatalf("slow query not captured: %+v", slow)
	}
}

func TestDescribeUnknownIndexIs404AndMaintenanceValidatesAction(t *testing.T) {
	handler, _ := newConsoleHandler("", time.Second)

	if code := call(handler, "GET", "/indices/ghost", "", "").Code; code != http.StatusNotFound {
		t.Fatalf("want 404, got %d", code)
	}

	if code := call(handler, "POST", "/indices/products/_maintenance/explode", "", "").Code; code != http.StatusBadRequest {
		t.Fatalf("want 400, got %d", code)
	}

	if code := call(handler, "POST", "/indices/products/_maintenance/flush", "", "").Code; code != http.StatusOK {
		t.Fatalf("want 200, got %d", code)
	}
}

func TestLatencyBucketsAreCumulative(t *testing.T) {
	metrics := newSearchMetrics(time.Hour)
	metrics.record("i", 3*time.Millisecond, 200, "")
	metrics.record("i", 40*time.Millisecond, 500, "")

	stats := metrics.snapshot()
	byBound := map[float64]uint64{}

	for _, bucket := range stats.Buckets {
		byBound[bucket.UpperBoundMillis] = bucket.Count
	}

	if byBound[5] != 1 || byBound[50] != 2 || stats.SearchErrors != 1 {
		t.Fatalf("unexpected buckets/errors: %+v", stats)
	}
}

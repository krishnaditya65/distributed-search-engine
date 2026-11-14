package api

import (
	"sync"
	"time"
)

var latencyBucketsMillis = []float64{1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500}

const (
	slowQueryLogCapacity = 100
	slowQueryBodyLimit   = 1024
)

type SlowQuery struct {
	Time       time.Time `json:"time"`
	Index      string    `json:"index"`
	TookMillis float64   `json:"took_ms"`
	Status     int       `json:"status"`
	Query      string    `json:"query"`
}

type LatencyBucket struct {
	UpperBoundMillis float64 `json:"le_ms"`
	Count            uint64  `json:"count"` // cumulative, Prometheus-style
}

type SearchStats struct {
	UptimeSeconds   int64             `json:"uptime_seconds"`
	SearchTotal     uint64            `json:"search_total"`
	SearchErrors    uint64            `json:"search_errors"`
	SlowThresholdMs float64           `json:"slow_threshold_ms"`
	LatencySumMs    float64           `json:"latency_sum_ms"`
	Buckets         []LatencyBucket   `json:"buckets"`
	SearchesByIndex map[string]uint64 `json:"searches_by_index"`
}

// searchMetrics records latency, errors and slow queries for the search endpoint.
type searchMetrics struct {
	mutex         sync.Mutex
	startedAt     time.Time
	slowThreshold time.Duration
	total, errors uint64
	latencySumMs  float64
	bucketCounts  []uint64 // non-cumulative, last slot = overflow
	byIndex       map[string]uint64
	slow          []SlowQuery
}

func newSearchMetrics(slowThreshold time.Duration) *searchMetrics {
	return &searchMetrics{startedAt: time.Now(), slowThreshold: slowThreshold, bucketCounts: make([]uint64, len(latencyBucketsMillis)+1), byIndex: map[string]uint64{}}
}

func (m *searchMetrics) record(index string, elapsed time.Duration, status int, query string) {
	millis := float64(elapsed.Microseconds()) / 1000

	m.mutex.Lock()
	defer m.mutex.Unlock()

	m.total++
	m.latencySumMs += millis
	m.byIndex[index]++

	if status >= 500 {
		m.errors++
	}

	m.bucketCounts[bucketIndexFor(millis)]++

	if elapsed >= m.slowThreshold {
		m.slow = append([]SlowQuery{{Time: time.Now(), Index: index, TookMillis: millis, Status: status, Query: query}}, m.slow...)
		if len(m.slow) > slowQueryLogCapacity {
			m.slow = m.slow[:slowQueryLogCapacity]
		}
	}
}

func bucketIndexFor(millis float64) int {
	for index, bound := range latencyBucketsMillis {
		if millis <= bound {
			return index
		}
	}

	return len(latencyBucketsMillis)
}

func (m *searchMetrics) snapshot() SearchStats {
	m.mutex.Lock()
	defer m.mutex.Unlock()

	buckets := make([]LatencyBucket, 0, len(latencyBucketsMillis))
	cumulative := uint64(0)

	for index, bound := range latencyBucketsMillis {
		cumulative += m.bucketCounts[index]
		buckets = append(buckets, LatencyBucket{UpperBoundMillis: bound, Count: cumulative})
	}

	byIndex := make(map[string]uint64, len(m.byIndex))
	for name, count := range m.byIndex {
		byIndex[name] = count
	}

	return SearchStats{
		UptimeSeconds: int64(time.Since(m.startedAt).Seconds()), SearchTotal: m.total, SearchErrors: m.errors,
		SlowThresholdMs: float64(m.slowThreshold.Milliseconds()), LatencySumMs: m.latencySumMs, Buckets: buckets, SearchesByIndex: byIndex,
	}
}

func (m *searchMetrics) slowQueries() []SlowQuery {
	m.mutex.Lock()
	defer m.mutex.Unlock()

	return append([]SlowQuery{}, m.slow...)
}

#pragma once

#include <string>

#include "llmgate/metrics/metrics.h"
#include "llmgate/resilience/circuit_breaker.h"
#include "llmgate/util/clock.h"

namespace llmgate {

// The metrics LLMGate publishes, with one small method per event so the
// handler and the router do not need to know any metric names.
//
//   llmgate_requests_total{status}                requests to /v1/chat, by HTTP status
//   llmgate_request_duration_seconds              how long /v1/chat requests took
//   llmgate_cache_hits_total / _misses_total      answered from the cache or not
//   llmgate_rate_limited_total                    requests refused with 429 rate_limited
//   llmgate_budget_rejected_total                 requests refused with 429 budget_exceeded
//   llmgate_provider_duration_seconds{provider}   how long calls to each provider took
//   llmgate_provider_errors_total{provider}       calls that failed, per provider
//   llmgate_breaker_state{provider}               0 closed, 1 open, 2 half-open
//   llmgate_failovers_total                       times a request moved to the next provider
//   llmgate_tokens_total{team}                    tokens used by each team (not counting cache hits)
//
// Labels come only from the configuration (team and provider names), never from
// request data: a label with unbounded values (keys, prompts) would create
// endless series and bury the metrics server.
class GatewayMetrics {
public:
    explicit GatewayMetrics(Clock clock = real_clock());

    MetricsRegistry& registry() { return registry_; }
    const MetricsRegistry& registry() const { return registry_; }

    // The time source (injectable, like everywhere else).
    TimePoint now() const { return clock_(); }
    static double seconds_between(TimePoint from, TimePoint to);

    void request_finished(int status, double seconds);
    void cache_hit() { cache_hits_.inc(); }
    void cache_miss() { cache_misses_.inc(); }
    void rate_limited() { rate_limited_.inc(); }
    void budget_rejected() { budget_rejected_.inc(); }
    void failover() { failovers_.inc(); }
    void provider_call(const std::string& provider, double seconds, bool ok);
    void tokens(const std::string& team, long long count);
    void set_breaker_state(const std::string& provider, BreakerState state);

    // The page served by GET /metrics.
    std::string render() const { return registry_.render(); }

private:
    Clock clock_;
    MetricsRegistry registry_;
    Histogram& request_duration_;
    Counter& cache_hits_;
    Counter& cache_misses_;
    Counter& rate_limited_;
    Counter& budget_rejected_;
    Counter& failovers_;
};

}  // namespace llmgate

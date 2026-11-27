#include "llmgate/metrics/gateway_metrics.h"

#include <chrono>

namespace llmgate {

GatewayMetrics::GatewayMetrics(Clock clock)
    : clock_(std::move(clock)),
      // Created now, so they show up as 0 before the first event happens.
      request_duration_(registry_.histogram("llmgate_request_duration_seconds",
                                            "Time to answer a /v1/chat request in seconds")),
      cache_hits_(registry_.counter("llmgate_cache_hits_total",
                                    "Chat requests answered from the cache")),
      cache_misses_(registry_.counter("llmgate_cache_misses_total",
                                      "Chat requests not found in the cache")),
      rate_limited_(registry_.counter("llmgate_rate_limited_total",
                                      "Chat requests refused because of the rate limit")),
      budget_rejected_(registry_.counter("llmgate_budget_rejected_total",
                                         "Chat requests refused because the daily token budget is used up")),
      failovers_(registry_.counter("llmgate_failovers_total",
                                   "Times a request moved on to the next provider after a failure")) {}

double GatewayMetrics::seconds_between(TimePoint from, TimePoint to) {
    return std::chrono::duration<double>(to - from).count();
}

void GatewayMetrics::request_finished(int status, double seconds) {
    registry_.counter("llmgate_requests_total", "Chat requests by HTTP status",
                      label("status", std::to_string(status)))
        .inc();
    request_duration_.observe(seconds);
}

void GatewayMetrics::provider_call(const std::string& provider, double seconds, bool ok) {
    const std::string provider_label = label("provider", provider);
    registry_.histogram("llmgate_provider_duration_seconds",
                        "Time a provider took to answer, in seconds", provider_label)
        .observe(seconds);
    if (!ok) {
        registry_.counter("llmgate_provider_errors_total", "Provider calls that failed",
                          provider_label)
            .inc();
    }
}

void GatewayMetrics::tokens(const std::string& team, long long count) {
    if (count <= 0) return;
    registry_.counter("llmgate_tokens_total", "Tokens used by providers, by team",
                      label("team", team))
        .inc(static_cast<std::uint64_t>(count));
}

void GatewayMetrics::set_breaker_state(const std::string& provider, BreakerState state) {
    double value = 0;  // closed
    if (state == BreakerState::Open) value = 1;
    else if (state == BreakerState::HalfOpen) value = 2;
    registry_.gauge("llmgate_breaker_state",
                    "Circuit breaker state per provider: 0 closed, 1 open, 2 half-open",
                    label("provider", provider))
        .set(value);
}

}  // namespace llmgate

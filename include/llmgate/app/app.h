#pragma once

#include <memory>
#include <string>
#include <vector>

#include "llmgate/auth/key_store.h"
#include "llmgate/budget/budget_tracker.h"
#include "llmgate/config/config.h"
#include "llmgate/handlers/chat_handler.h"
#include "llmgate/handlers/usage_handler.h"
#include "llmgate/limiter/token_bucket.h"
#include "llmgate/metrics/gateway_metrics.h"
#include "llmgate/provider/router.h"
#include "llmgate/resilience/retry.h"
#include "llmgate/server/router.h"
#include "llmgate/util/clock.h"
#include "llmgate/util/logger.h"

namespace llmgate {

// Builds every part of the gateway from a Config and connects them:
// key store, rate limiter, cache, provider router (the providers, each with its
// own circuit breaker), budget tracker, chat and usage handlers, and the HTTP
// router. Both main() and the integration tests use it, so what the tests
// exercise is exactly what runs in production.
class App {
public:
    explicit App(const Config& config, Clock clock = real_clock(),
                 WallClock wall_clock = real_wall_clock(), Sleeper sleeper = real_sleeper(),
                 Logger* logger = nullptr);

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Dispatches a request: GET /health, GET /metrics, POST /v1/chat and GET /v1/usage.
    const Router& router() const { return router_; }

    // False if no usable provider or no keys were configured. /health still works.
    bool chat_enabled() const { return chat_ != nullptr; }

    // The providers in the order they are tried, with failover counters
    // (used by the metrics endpoint in a later task).
    const ProviderRouter& providers() const { return providers_; }

    // Everything GET /metrics publishes.
    const GatewayMetrics& metrics() const { return metrics_; }

    // Human-readable messages about choices made at startup
    // (for example "using provider 'ollama' (ollama), priority 1").
    const std::vector<std::string>& notes() const { return notes_; }

private:
    std::vector<std::string> notes_;
    KeyStore keys_;
    RateLimiter limiter_;
    ChatCache cache_;
    GatewayMetrics metrics_;
    ProviderRouter providers_;
    BudgetTracker budget_;
    std::unique_ptr<ChatHandler> chat_;
    std::unique_ptr<UsageHandler> usage_;
    Router router_;
};

}  // namespace llmgate

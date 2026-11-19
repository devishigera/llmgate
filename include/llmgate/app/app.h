#pragma once

#include <memory>
#include <string>
#include <vector>

#include "llmgate/auth/key_store.h"
#include "llmgate/config/config.h"
#include "llmgate/handlers/chat_handler.h"
#include "llmgate/limiter/token_bucket.h"
#include "llmgate/provider/provider.h"
#include "llmgate/resilience/circuit_breaker.h"
#include "llmgate/server/router.h"
#include "llmgate/util/clock.h"

namespace llmgate {

// Builds every part of the gateway from a Config and connects them:
// key store, rate limiter, cache, provider, circuit breaker, chat handler and
// the router. Both main() and the integration tests use it, so what the tests
// exercise is exactly what runs in production.
class App {
public:
    explicit App(const Config& config, Clock clock = real_clock());

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Dispatches a request: GET /health and POST /v1/chat.
    const Router& router() const { return router_; }

    // False if no usable provider or no keys were configured. /health still works.
    bool chat_enabled() const { return chat_ != nullptr; }

    // Human-readable messages about choices made at startup
    // (for example "provider 'ollama' is not available yet, skipping").
    const std::vector<std::string>& notes() const { return notes_; }

private:
    std::vector<std::string> notes_;
    KeyStore keys_;
    RateLimiter limiter_;
    ChatCache cache_;
    std::unique_ptr<Provider> provider_;
    CircuitBreaker breaker_;
    std::unique_ptr<ChatHandler> chat_;
    Router router_;
};

}  // namespace llmgate
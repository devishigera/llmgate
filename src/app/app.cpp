#include "llmgate/app/app.h"

#include <chrono>

#include "llmgate/health.h"
#include "llmgate/provider/mock.h"
#include "llmgate/provider/ollama.h"

namespace llmgate {

namespace {

// Builds the provider a config entry describes, or nullptr for an unknown type.
std::unique_ptr<Provider> make_provider(const ProviderConfig& p) {
    if (p.type == "ollama") {
        OllamaOptions options;
        options.name = p.name;
        options.url = p.url;
        return std::make_unique<OllamaProvider>(options);
    }
    if (p.type == "mock") {
        MockOptions options;
        options.name = p.name;
        return std::make_unique<MockProvider>(options);
    }
    return nullptr;
}

}  // namespace

App::App(const Config& config, Clock clock, WallClock wall_clock, Sleeper sleeper)
    : keys_(config.keys),
      limiter_(clock),
      cache_(config.cache.capacity, std::chrono::seconds(config.cache.ttl_seconds), clock),
      providers_(RetryPolicy{}, std::move(sleeper), clock),
      budget_(std::move(wall_clock)) {
    router_.add("GET", "/health", [](const HttpRequest&) {
        return json_response(200, health_json());
    });

    // One provider (with its own circuit breaker) per config entry. The router
    // tries them in priority order and fails over when one is down.
    for (const auto& p : config.providers) {
        std::unique_ptr<Provider> provider = make_provider(p);
        if (!provider) {
            notes_.push_back("provider '" + p.name + "' has unknown type '" + p.type + "', skipping");
            continue;
        }
        auto breaker = std::make_unique<CircuitBreaker>(
            config.circuit_breaker.failure_threshold,
            std::chrono::seconds(config.circuit_breaker.cool_down_seconds),
            /*half_open_trials=*/1, clock);
        providers_.add(std::move(provider), std::move(breaker), p.priority,
                       std::chrono::milliseconds(p.timeout_ms));
        notes_.push_back("using provider '" + p.name + "' (" + p.type + "), priority " +
                         std::to_string(p.priority));
    }

    if (keys_.size() == 0) {
        notes_.push_back("no API keys configured: /v1/chat is disabled");
        return;
    }

    usage_ = std::make_unique<UsageHandler>(keys_, budget_);
    router_.add("GET", "/v1/usage", [this](const HttpRequest& request) {
        return usage_->handle(request);
    });

    if (providers_.size() == 0) {
        notes_.push_back("no usable provider: /v1/chat is disabled");
        return;
    }

    chat_ = std::make_unique<ChatHandler>(keys_, limiter_, cache_, providers_, &budget_);
    router_.add("POST", "/v1/chat", [this](const HttpRequest& request) {
        return chat_->handle(request);
    });
}

}  // namespace llmgate

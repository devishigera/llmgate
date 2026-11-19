#include "llmgate/app/app.h"

#include <algorithm>
#include <chrono>

#include "llmgate/health.h"
#include "llmgate/provider/mock.h"

namespace llmgate {

namespace {

// Picks the provider with the lowest priority number that we can actually
// run. Only "mock" exists for now; real providers arrive in Phase 2.
// Returns nullptr if none is usable.
std::unique_ptr<Provider> pick_provider(const Config& config, std::chrono::milliseconds& timeout,
                                        std::vector<std::string>& notes) {
    std::vector<const ProviderConfig*> ordered;
    for (const auto& p : config.providers) ordered.push_back(&p);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const ProviderConfig* a, const ProviderConfig* b) {
                         return a->priority < b->priority;
                     });

    for (const ProviderConfig* p : ordered) {
        if (p->type == "mock") {
            MockOptions options;
            options.name = p->name;
            timeout = std::chrono::milliseconds(p->timeout_ms);
            notes.push_back("using provider '" + p->name + "' (mock)");
            return std::make_unique<MockProvider>(options);
        }
        notes.push_back("provider '" + p->name + "' (type " + p->type +
                        ") is not available yet, skipping");
    }
    return nullptr;
}

}  // namespace

App::App(const Config& config, Clock clock)
    : keys_(config.keys),
      limiter_(clock),
      cache_(config.cache.capacity, std::chrono::seconds(config.cache.ttl_seconds), clock),
      breaker_(config.circuit_breaker.failure_threshold,
               std::chrono::seconds(config.circuit_breaker.cool_down_seconds),
               /*half_open_trials=*/1, clock) {
    router_.add("GET", "/health", [](const HttpRequest&) {
        return json_response(200, health_json());
    });

    std::chrono::milliseconds timeout(30000);
    provider_ = pick_provider(config, timeout, notes_);

    if (keys_.size() == 0) {
        notes_.push_back("no API keys configured: /v1/chat is disabled");
        return;
    }
    if (!provider_) {
        notes_.push_back("no usable provider: /v1/chat is disabled");
        return;
    }

    chat_ = std::make_unique<ChatHandler>(keys_, limiter_, cache_, *provider_, breaker_, timeout);
    router_.add("POST", "/v1/chat", [this](const HttpRequest& request) {
        return chat_->handle(request);
    });
}

}  // namespace llmgate
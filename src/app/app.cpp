#include "llmgate/app/app.h"

#include <algorithm>
#include <chrono>

#include "llmgate/health.h"
#include "llmgate/provider/mock.h"
#include "llmgate/provider/ollama.h"

namespace llmgate {

namespace {

// Picks the provider with the lowest priority number. Types: "mock" and
// "ollama". Returns nullptr if the config has no providers.
std::unique_ptr<Provider> pick_provider(const Config& config, std::chrono::milliseconds& timeout,
                                        std::vector<std::string>& notes) {
    const ProviderConfig* best = nullptr;
    for (const auto& p : config.providers) {
        if (best == nullptr || p.priority < best->priority) best = &p;
    }
    if (best == nullptr) return nullptr;

    timeout = std::chrono::milliseconds(best->timeout_ms);
    if (best->type == "ollama") {
        OllamaOptions options;
        options.name = best->name;
        options.url = best->url;
        notes.push_back("using provider '" + best->name + "' (ollama)");
        return std::make_unique<OllamaProvider>(options);
    }

    if (best->type == "mock") {
        MockOptions options;
        options.name = best->name;
        notes.push_back("using provider '" + best->name + "' (mock)");
        return std::make_unique<MockProvider>(options);
    }

    notes.push_back("provider '" + best->name + "' has unknown type '" + best->type + "'");
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

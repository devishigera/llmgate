#include "llmgate/provider/mock.h"

#include <thread>

namespace llmgate {

int estimate_tokens(const std::string& text) {
    return static_cast<int>((text.size() + 3) / 4);
}

MockProvider::MockProvider(MockOptions options)
    : options_(std::move(options)), rng_(options_.seed) {}

void MockProvider::set_fail_next(int n) {
    std::lock_guard<std::mutex> lock(mutex_);
    options_.fail_next_n = n;
}

void MockProvider::set_fail_rate(double rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    options_.fail_rate = rate;
}

int MockProvider::calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return calls_;
}

ChatResponse MockProvider::chat(const ChatRequest& request,
                                std::chrono::milliseconds timeout) {
    bool fail = false;
    int latency_ms = 0;
    std::string reply;
    {
        // Decide everything under the lock, then release it before sleeping
        // so a slow call never blocks other callers.
        std::lock_guard<std::mutex> lock(mutex_);
        ++calls_;
        latency_ms = options_.latency_ms;
        reply = options_.canned_reply;
        if (options_.fail_next_n > 0) {
            --options_.fail_next_n;
            fail = true;
        } else if (options_.fail_rate > 0.0) {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            fail = dist(rng_) < options_.fail_rate;
        }
    }

    if (latency_ms > 0) {
        // If the pretend latency is longer than the allowed time, wait only
        // for the allowed time and then report a timeout.
        const auto wait = std::chrono::milliseconds(latency_ms);
        if (wait > timeout) {
            std::this_thread::sleep_for(timeout);
            throw ProviderError(options_.name + ": timed out", /*transient=*/true,
                                /*timeout=*/true);
        }
        std::this_thread::sleep_for(wait);
    }

    if (fail) {
        throw ProviderError(options_.name + ": injected failure", /*transient=*/true);
    }

    ChatResponse response;
    response.model = request.model;
    response.provider = options_.name;
    response.content = reply;

    int prompt_tokens = 0;
    for (const auto& m : request.messages) prompt_tokens += estimate_tokens(m.content);
    response.usage.prompt_tokens = prompt_tokens;
    response.usage.completion_tokens = estimate_tokens(reply);
    return response;
}

}  // namespace llmgate
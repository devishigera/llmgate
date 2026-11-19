#pragma once

#include <mutex>
#include <random>
#include <string>

#include "llmgate/provider/provider.h"

namespace llmgate {

struct MockOptions {
    std::string name = "mock";
    std::string canned_reply = "This is a mock reply.";
    int fail_next_n = 0;        // the next N calls fail, then normal behaviour
    double fail_rate = 0.0;     // 0.0 never fails, 1.0 always fails
    int latency_ms = 0;         // pretend the model is slow (tests keep this 0)
    unsigned seed = 12345;      // makes fail_rate reproducible
};

// A fake provider that needs no network. It can fail or be slow on command,
// which lets us test retries, fallback and the circuit breaker.
class MockProvider : public Provider {
public:
    explicit MockProvider(MockOptions options = {});

    ChatResponse chat(const ChatRequest& request,
                      std::chrono::milliseconds timeout) override;
    std::string name() const override { return options_.name; }

    // Change behaviour while running (used by tests and by later tasks).
    void set_fail_next(int n);
    void set_fail_rate(double rate);
    int calls() const;

private:
    MockOptions options_;
    mutable std::mutex mutex_;
    std::mt19937 rng_;
    int calls_ = 0;
};

// Rough token estimate used by the mock: about 4 characters per token,
// rounded up, and at least 1 for any non-empty text.
int estimate_tokens(const std::string& text);

}  // namespace llmgate
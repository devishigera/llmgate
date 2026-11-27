#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "llmgate/metrics/gateway_metrics.h"
#include "llmgate/provider/provider.h"
#include "llmgate/resilience/circuit_breaker.h"
#include "llmgate/resilience/retry.h"
#include "llmgate/util/clock.h"

namespace llmgate {

// Thrown by ProviderRouter::chat() when no provider could even be tried,
// because every circuit breaker is open (or there are no providers at all).
// ChatHandler turns it into 502 "provider_unavailable".
class NoProviderAvailable : public ProviderError {
public:
    NoProviderAvailable() : ProviderError("no provider is available", /*transient=*/true) {}
};

struct ProviderStatus {
    std::string name;
    int priority = 0;
    BreakerState breaker = BreakerState::Closed;
};

// Sends a chat request to several providers, best (lowest priority number)
// first, and handles the three ways a provider can let us down:
//
//   - its circuit breaker is open        -> skip it without calling it
//   - the call fails with a transient error -> retry it, with backoff (retry.h)
//   - it still fails                     -> record the failure and fail over to
//                                           the next provider
//
// If every provider fails, the LAST error is thrown (so a timeout stays a
// timeout and can become a 504). If none could be tried, NoProviderAvailable.
//
// Each provider call counts as ONE success or failure for its circuit breaker,
// however many retries it took inside.
//
// Add providers once at startup, before serving. After that chat(), status()
// and failover_count() are thread-safe.
class ProviderRouter {
public:
    // sleeper/clock/seed are injectable so tests are instant and repeatable.
    explicit ProviderRouter(RetryPolicy retry = {}, Sleeper sleeper = real_sleeper(),
                            Clock clock = real_clock(),
                            std::uint64_t seed = std::random_device{}());

    // The router takes ownership.
    void add(std::unique_ptr<Provider> provider, std::unique_ptr<CircuitBreaker> breaker,
             int priority, std::chrono::milliseconds timeout);

    // The router only borrows them: the caller keeps them alive (used by tests).
    void add_ref(Provider& provider, CircuitBreaker& breaker, int priority,
                 std::chrono::milliseconds timeout);

    // Optional: count failovers and time provider calls. Call before serving.
    void set_metrics(GatewayMetrics* metrics) { metrics_ = metrics; }

    ChatResponse chat(const ChatRequest& request);

    // How many times a request moved on to the next provider after a failure.
    long long failover_count() const { return failovers_.load(); }

    // Providers in the order they are tried, with their breaker state.
    std::vector<ProviderStatus> status() const;

    std::size_t size() const { return entries_.size(); }

private:
    struct Entry {
        std::unique_ptr<Provider> owned_provider;
        std::unique_ptr<CircuitBreaker> owned_breaker;
        Provider* provider = nullptr;
        CircuitBreaker* breaker = nullptr;
        int priority = 0;
        std::chrono::milliseconds timeout{30000};
    };

    void insert(Entry entry);
    std::uint64_t next_seed();

    RetryPolicy retry_;
    Sleeper sleeper_;
    Clock clock_;
    std::vector<Entry> entries_;  // sorted by priority, ties keep insertion order

    std::mutex seed_mutex_;
    std::mt19937_64 seed_source_;
    std::atomic<long long> failovers_{0};
    GatewayMetrics* metrics_ = nullptr;
};

}  // namespace llmgate

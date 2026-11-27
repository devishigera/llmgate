#include "llmgate/provider/router.h"

#include <algorithm>
#include <exception>
#include <optional>

namespace llmgate {

ProviderRouter::ProviderRouter(RetryPolicy retry, Sleeper sleeper, Clock clock, std::uint64_t seed)
    : retry_(retry),
      sleeper_(std::move(sleeper)),
      clock_(std::move(clock)),
      seed_source_(seed) {}

void ProviderRouter::insert(Entry entry) {
    // First position whose priority is larger: equal priorities keep the order they were added.
    auto pos = std::upper_bound(entries_.begin(), entries_.end(), entry.priority,
                                [](int priority, const Entry& e) { return priority < e.priority; });
    entries_.insert(pos, std::move(entry));
}

void ProviderRouter::add(std::unique_ptr<Provider> provider, std::unique_ptr<CircuitBreaker> breaker,
                         int priority, std::chrono::milliseconds timeout) {
    Entry e;
    e.provider = provider.get();
    e.breaker = breaker.get();
    e.owned_provider = std::move(provider);
    e.owned_breaker = std::move(breaker);
    e.priority = priority;
    e.timeout = timeout;
    insert(std::move(e));
}

void ProviderRouter::add_ref(Provider& provider, CircuitBreaker& breaker, int priority,
                             std::chrono::milliseconds timeout) {
    Entry e;
    e.provider = &provider;
    e.breaker = &breaker;
    e.priority = priority;
    e.timeout = timeout;
    insert(std::move(e));
}

std::uint64_t ProviderRouter::next_seed() {
    std::lock_guard<std::mutex> lock(seed_mutex_);
    return seed_source_();
}

ChatResponse ProviderRouter::chat(const ChatRequest& request) {
    // One private random engine per call: no sharing between threads, and
    // still repeatable because its seed comes from the seeded source.
    std::mt19937_64 rng(next_seed());

    std::optional<ProviderError> last_error;
    bool tried_any = false;

    for (Entry& entry : entries_) {
        if (!entry.breaker->allow_request()) continue;  // breaker open: skip, no call

        if (tried_any) {  // we are moving on from a provider that failed
            ++failovers_;
            if (metrics_ != nullptr) metrics_->failover();
        }
        tried_any = true;

        const TimePoint started = clock_();
        auto record_call = [&](bool ok) {
            if (metrics_ != nullptr) {
                metrics_->provider_call(entry.provider->name(),
                                        GatewayMetrics::seconds_between(started, clock_()), ok);
            }
        };

        // allow_request() must be followed by exactly one record_success()
        // or record_failure(), whatever happens inside.
        try {
            ChatResponse response = call_with_retry(
                retry_, [&] { return entry.provider->chat(request, entry.timeout); }, sleeper_,
                rng, clock_);
            entry.breaker->record_success();
            record_call(true);
            return response;
        } catch (const ProviderError& e) {
            entry.breaker->record_failure();
            record_call(false);
            // The message can contain text from the provider, so only the kind of failure is logged.
            if (logger_ != nullptr) {
                logger_->warn("provider_failed", {{"provider", entry.provider->name()},
                                                  {"transient", e.transient()},
                                                  {"timeout", e.timeout()}});
            }
            last_error = e;
        } catch (const std::exception&) {
            // Something that is not a ProviderError: a bug. Never show its text to clients.
            entry.breaker->record_failure();
            record_call(false);
            if (logger_ != nullptr) {
                logger_->error("provider_crashed", {{"provider", entry.provider->name()}});
            }
            last_error = ProviderError("unexpected provider failure", /*transient=*/false);
        }
    }

    if (last_error) throw *last_error;
    throw NoProviderAvailable();
}

std::vector<ProviderStatus> ProviderRouter::status() const {
    std::vector<ProviderStatus> out;
    for (const Entry& e : entries_) {
        out.push_back({e.provider->name(), e.priority, e.breaker->state()});
    }
    return out;
}

}  // namespace llmgate

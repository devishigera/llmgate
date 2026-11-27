#pragma once

#include <chrono>

#include "llmgate/auth/key_store.h"
#include "llmgate/budget/budget_tracker.h"
#include "llmgate/cache/lru_cache.h"
#include "llmgate/limiter/token_bucket.h"
#include "llmgate/metrics/gateway_metrics.h"
#include "llmgate/provider/router.h"
#include "llmgate/server/http_types.h"
#include "llmgate/util/logger.h"

namespace llmgate {

using ChatCache = LruCache<std::string, ChatResponse>;

// Handles POST /v1/chat. Every request goes through the same pipeline:
//
//   1. authenticate the API key            -> 401 unauthorized
//   2. parse and validate the JSON body    -> 400 bad_request
//   3. rate limit this key                 -> 429 rate_limited (+ retry-after)
//   3b. daily token budget (if enabled)    -> 429 budget_exceeded (+ retry-after)
//   4. look in the cache                   -> 200 (cached: true)
//   5. ask the provider router            -> 200 (cached: false)
//        (circuit breaker, retry and failover happen inside the router)
//                                             502 provider_unavailable (none could be tried)
//                                             502 provider_error / 504 provider_timeout
//
// The handler only uses references, so whoever creates it (main, or a test)
// owns the dependencies and must keep them alive as long as the handler.
class ChatHandler {
public:
    ChatHandler(const KeyStore& keys, RateLimiter& limiter, ChatCache& cache,
                ProviderRouter& router, BudgetTracker* budget = nullptr,
                GatewayMetrics* metrics = nullptr, Logger* logger = nullptr);

    // Gives every response an x-request-id header and (if there is a logger)
    // writes one log line per request.
    HttpResponse handle(const HttpRequest& request);

private:
    // What the pipeline learned about a request, for the log line.
    struct RequestInfo {
        std::string team;      // empty until the key is accepted
        bool cache_hit = false;
        std::string provider;  // who produced the answer (empty if nobody did)
    };

    HttpResponse handle_impl(const HttpRequest& request, RequestInfo& info);

    const KeyStore& keys_;
    RateLimiter& limiter_;
    ChatCache& cache_;
    ProviderRouter& router_;
    BudgetTracker* budget_;    // may be null: budgets are not enforced then
    GatewayMetrics* metrics_;  // may be null: nothing is measured then
    Logger* logger_;           // may be null: nothing is logged then
};

}  // namespace llmgate

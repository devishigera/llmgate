#pragma once

#include <chrono>

#include "llmgate/auth/key_store.h"
#include "llmgate/budget/budget_tracker.h"
#include "llmgate/cache/lru_cache.h"
#include "llmgate/limiter/token_bucket.h"
#include "llmgate/provider/router.h"
#include "llmgate/server/http_types.h"

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
                ProviderRouter& router, BudgetTracker* budget = nullptr);

    HttpResponse handle(const HttpRequest& request);

private:
    const KeyStore& keys_;
    RateLimiter& limiter_;
    ChatCache& cache_;
    ProviderRouter& router_;
    BudgetTracker* budget_;  // may be null: budgets are not enforced then
};

}  // namespace llmgate

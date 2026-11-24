#pragma once

#include "llmgate/auth/key_store.h"
#include "llmgate/budget/budget_tracker.h"
#include "llmgate/server/http_types.h"

namespace llmgate {

// Handles GET /v1/usage: how many tokens the calling key has used today and
// what its daily budget is.
//
//   {"used_today": 1234, "daily_budget": 100000}
//
// Needs a valid API key (401 otherwise). It does not use up rate limit or budget.
class UsageHandler {
public:
    UsageHandler(const KeyStore& keys, const BudgetTracker& budget);

    HttpResponse handle(const HttpRequest& request) const;

private:
    const KeyStore& keys_;
    const BudgetTracker& budget_;
};

}  // namespace llmgate

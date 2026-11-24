#include "llmgate/handlers/usage_handler.h"

#include <nlohmann/json.hpp>

namespace llmgate {

UsageHandler::UsageHandler(const KeyStore& keys, const BudgetTracker& budget)
    : keys_(keys), budget_(budget) {}

HttpResponse UsageHandler::handle(const HttpRequest& request) const {
    const auto token = parse_bearer(request.header("authorization"));
    const KeyConfig* key = token ? keys_.authenticate(*token) : nullptr;
    if (key == nullptr) {
        HttpResponse r = error_response(401, "unauthorized",
                                        "missing or invalid API key; send "
                                        "\"Authorization: Bearer <key>\"");
        r.headers["www-authenticate"] = "Bearer";
        return r;
    }

    const nlohmann::json body = {
        {"used_today", budget_.used_today(key->key_sha256)},
        {"daily_budget", key->daily_token_budget},
    };
    return json_response(200, body.dump());
}

}  // namespace llmgate

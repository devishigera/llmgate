#include "llmgate/handlers/chat_handler.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <string>

#include "llmgate/util/sha256.h"

namespace llmgate {

namespace {

using json = nlohmann::json;

constexpr int kDefaultMaxTokens = 256;
constexpr int kMaxAllowedTokens = 8192;

// What the client asked for, after validation.
struct ParsedRequest {
    ChatRequest chat;
};

// Returns an empty string if the body is fine, otherwise a message that says
// what is wrong. Fills `out` on success.
std::string parse_body(const std::string& body, ParsedRequest& out) {
    json j;
    try {
        j = json::parse(body);
    } catch (const json::parse_error&) {
        return "the body is not valid JSON";
    }
    if (!j.is_object()) return "the body must be a JSON object";

    auto model = j.find("model");
    if (model == j.end() || !model->is_string() || model->get<std::string>().empty()) {
        return "\"model\" must be a non-empty string";
    }
    out.chat.model = model->get<std::string>();

    auto messages = j.find("messages");
    if (messages == j.end() || !messages->is_array() || messages->empty()) {
        return "\"messages\" must be a non-empty array";
    }
    for (std::size_t i = 0; i < messages->size(); ++i) {
        const json& m = (*messages)[i];
        const std::string where = "messages[" + std::to_string(i) + "]";
        if (!m.is_object()) return where + " must be an object";

        auto role = m.find("role");
        if (role == m.end() || !role->is_string()) return where + ".role must be a string";
        const std::string r = role->get<std::string>();
        if (r != "system" && r != "user" && r != "assistant") {
            return where + ".role must be \"system\", \"user\" or \"assistant\"";
        }

        auto content = m.find("content");
        if (content == m.end() || !content->is_string()) {
            return where + ".content must be a string";
        }
        out.chat.messages.push_back({r, content->get<std::string>()});
    }

    out.chat.max_tokens = kDefaultMaxTokens;
    if (auto it = j.find("max_tokens"); it != j.end()) {
        if (!it->is_number_integer()) return "\"max_tokens\" must be a whole number";
        const long long v = it->get<long long>();
        if (v < 1 || v > kMaxAllowedTokens) {
            return "\"max_tokens\" must be between 1 and " + std::to_string(kMaxAllowedTokens);
        }
        out.chat.max_tokens = static_cast<int>(v);
    }

    if (auto it = j.find("stream"); it != j.end()) {
        if (!it->is_boolean()) return "\"stream\" must be true or false";
        if (it->get<bool>()) return "streaming is not supported yet; send \"stream\": false";
    }
    return "";
}

// Same question from the same team gives the same key. The team is part of
// the key so one team can never be served another team's cached answer.
std::string cache_key(const std::string& team, const ChatRequest& chat) {
    json messages = json::array();
    for (const auto& m : chat.messages) {
        messages.push_back({{"role", m.role}, {"content", m.content}});
    }
    // dump() of a JSON array of objects is deterministic (object keys are sorted).
    return sha256_hex(team + "\n" + chat.model + "\n" + messages.dump() + "\n" +
                      std::to_string(chat.max_tokens));
}

// A cheap guess made before the call: about 4 characters per token, at least 1.
// After the call we record the real number from the provider instead.
long long estimate_request_tokens(const ChatRequest& chat) {
    long long chars = 0;
    for (const auto& m : chat.messages) chars += static_cast<long long>(m.content.size());
    return std::max(1LL, chars / 4);
}

HttpResponse success(const ChatResponse& r, bool cached) {
    json body = {
        {"model", r.model},
        {"provider", r.provider},
        {"content", r.content},
        {"usage",
         {{"prompt_tokens", r.usage.prompt_tokens},
          {"completion_tokens", r.usage.completion_tokens},
          {"total_tokens", r.usage.total()}}},
        {"cached", cached},
    };
    return json_response(200, body.dump());
}

}  // namespace

ChatHandler::ChatHandler(const KeyStore& keys, RateLimiter& limiter, ChatCache& cache,
                         Provider& provider, CircuitBreaker& breaker,
                         std::chrono::milliseconds provider_timeout, BudgetTracker* budget)
    : keys_(keys),
      limiter_(limiter),
      cache_(cache),
      provider_(provider),
      breaker_(breaker),
      provider_timeout_(provider_timeout),
      budget_(budget) {}

HttpResponse ChatHandler::handle(const HttpRequest& request) {
    // 1. Authenticate
    const auto token = parse_bearer(request.header("authorization"));
    const KeyConfig* key = token ? keys_.authenticate(*token) : nullptr;
    if (key == nullptr) {
        HttpResponse r = error_response(401, "unauthorized",
                                        "missing or invalid API key; send "
                                        "\"Authorization: Bearer <key>\"");
        r.headers["www-authenticate"] = "Bearer";
        return r;
    }

    // 2. Validate the body
    ParsedRequest parsed;
    const std::string problem = parse_body(request.body, parsed);
    if (!problem.empty()) return error_response(400, "bad_request", problem);

    // 3. Rate limit (one bucket per key)
    if (!limiter_.allow(key->key_sha256, key->rate_limit.burst, key->rate_limit.per_second)) {
        const double wait = limiter_.retry_after_seconds(key->key_sha256);
        const long long seconds = std::max(1LL, static_cast<long long>(std::ceil(wait)));
        HttpResponse r = error_response(429, "rate_limited",
                                        "too many requests; retry in " +
                                            std::to_string(seconds) + " second(s)");
        r.headers["retry-after"] = std::to_string(seconds);
        return r;
    }

    // 3b. Daily token budget. Checked before the cache, so a key that has
    //     used up its budget gets a clear 429 whatever it asks.
    if (budget_ != nullptr) {
        const long long estimate = estimate_request_tokens(parsed.chat);
        if (!budget_->can_spend(key->key_sha256, estimate, key->daily_token_budget)) {
            HttpResponse r = error_response(429, "budget_exceeded",
                                            "the daily token budget is used up; it resets at "
                                            "00:00 UTC");
            r.headers["retry-after"] = std::to_string(budget_->seconds_until_reset());
            return r;
        }
    }

    // 4. Cache (a cache hit costs no tokens)
    const std::string ckey = cache_key(key->team, parsed.chat);
    if (auto hit = cache_.get(ckey)) return success(*hit, /*cached=*/true);

    // 5. Circuit breaker
    if (!breaker_.allow_request()) {
        return error_response(502, "provider_unavailable",
                              "the model provider is temporarily unavailable");
    }

    // 6. Provider. Every allow_request() must be followed by exactly one
    //    record_success() or record_failure().
    try {
        ChatResponse response = provider_.chat(parsed.chat, provider_timeout_);
        breaker_.record_success();
        if (budget_ != nullptr) budget_->record(key->key_sha256, response.usage.total());
        cache_.put(ckey, response);
        return success(response, /*cached=*/false);
    } catch (const ProviderError& e) {
        breaker_.record_failure();
        if (e.timeout()) {
            return error_response(504, "provider_timeout", "the model provider took too long");
        }
        return error_response(502, "provider_error", "the model provider failed");
    } catch (const std::exception&) {
        breaker_.record_failure();
        return error_response(502, "provider_error", "the model provider failed");
    }
}

}  // namespace llmgate

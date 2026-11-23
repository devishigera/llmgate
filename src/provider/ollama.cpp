#include "llmgate/provider/ollama.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>

#include "llmgate/provider/mock.h"  // estimate_tokens

namespace llmgate {

namespace {

using json = nlohmann::json;

// Builds the text of an error from Ollama's {"error": "..."} body, if present.
std::string error_text(const std::string& body) {
    try {
        json j = json::parse(body);
        if (j.is_object()) {
            auto it = j.find("error");
            if (it != j.end() && it->is_string()) return it->get<std::string>();
        }
    } catch (const json::exception&) {
    }
    return "";
}

}  // namespace

OllamaProvider::OllamaProvider(OllamaOptions options) : options_(std::move(options)) {}

ChatResponse OllamaProvider::chat(const ChatRequest& request, std::chrono::milliseconds timeout) {
    // 1. Build the request body.
    json messages = json::array();
    for (const auto& m : request.messages) {
        messages.push_back({{"role", m.role}, {"content", m.content}});
    }
    const json body = {
        {"model", request.model},
        {"messages", messages},
        {"stream", false},
        {"options", {{"num_predict", request.max_tokens}}},
    };

    // 2. Send it. A new client per call keeps this class thread-safe.
    httplib::Client client(options_.url);
    client.set_connection_timeout(timeout);
    client.set_read_timeout(timeout);
    client.set_write_timeout(timeout);

    const auto started = std::chrono::steady_clock::now();
    httplib::Result result = client.Post("/api/chat", body.dump(), "application/json");
    const auto elapsed = std::chrono::steady_clock::now() - started;

    // 3. No answer at all: could not connect, or we waited too long.
    if (!result) {
        // The library reports a timeout as a read or connection error, so we
        // decide by the clock: close to the limit means it was a timeout.
        const bool timed_out = elapsed >= (timeout * 9) / 10;
        if (timed_out) {
            throw ProviderError(options_.name + ": timed out after " +
                                    std::to_string(timeout.count()) + " ms",
                                /*transient=*/true, /*timeout=*/true);
        }
        throw ProviderError(options_.name + ": cannot reach " + options_.url,
                            /*transient=*/true);
    }

    // 4. An answer, but an error status.
    const int status = result->status;
    if (status < 200 || status >= 300) {
        std::string message = options_.name + ": HTTP " + std::to_string(status);
        const std::string detail = error_text(result->body);
        if (!detail.empty()) message += ": " + detail;

        const bool transient = status >= 500 || status == 408 || status == 429;
        throw ProviderError(message, transient);
    }

    // 5. Read the answer.
    json reply;
    try {
        reply = json::parse(result->body);
    } catch (const json::parse_error&) {
        throw ProviderError(options_.name + ": reply is not valid JSON", false);
    }
    if (!reply.is_object()) {
        throw ProviderError(options_.name + ": reply is not a JSON object", false);
    }
    if (auto err = reply.find("error"); err != reply.end() && err->is_string()) {
        throw ProviderError(options_.name + ": " + err->get<std::string>(), false);
    }

    auto message = reply.find("message");
    if (message == reply.end() || !message->is_object()) {
        throw ProviderError(options_.name + ": reply has no \"message\"", false);
    }
    auto content = message->find("content");
    if (content == message->end() || !content->is_string()) {
        throw ProviderError(options_.name + ": reply has no message content", false);
    }

    ChatResponse response;
    response.provider = options_.name;
    response.content = content->get<std::string>();
    response.model = request.model;
    if (auto m = reply.find("model"); m != reply.end() && m->is_string()) {
        response.model = m->get<std::string>();
    }

    // Token counts: use Ollama's numbers, or estimate when it leaves them out
    // (it can omit the prompt count when the prompt was cached).
    int prompt_estimate = 0;
    for (const auto& m : request.messages) prompt_estimate += estimate_tokens(m.content);

    auto count = [&reply](const char* field, int fallback) {
        auto it = reply.find(field);
        if (it != reply.end() && it->is_number_integer()) {
            return static_cast<int>(std::max<long long>(0, it->get<long long>()));
        }
        return fallback;
    };
    response.usage.prompt_tokens = count("prompt_eval_count", prompt_estimate);
    response.usage.completion_tokens = count("eval_count", estimate_tokens(response.content));
    return response;
}

}  // namespace llmgate

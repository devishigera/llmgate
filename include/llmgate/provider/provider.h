#pragma once

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

namespace llmgate {

struct Message {
    std::string role;     // "system", "user" or "assistant"
    std::string content;
};

struct ChatRequest {
    std::string model;
    std::vector<Message> messages;
    int max_tokens = 256;
    bool stream = false;
};

struct Usage {
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total() const { return prompt_tokens + completion_tokens; }
};

struct ChatResponse {
    std::string model;
    std::string provider;  // which provider answered
    std::string content;
    Usage usage;
};

// Thrown by a provider when a call fails.
//   transient(): retrying (or trying another provider) could help,
//                for example a timeout or a 5xx answer.
//   timeout():   the call took longer than the allowed time.
class ProviderError : public std::runtime_error {
public:
    ProviderError(const std::string& message, bool transient, bool timeout = false)
        : std::runtime_error(message), transient_(transient), timeout_(timeout) {}

    bool transient() const { return transient_; }
    bool timeout() const { return timeout_; }

private:
    bool transient_;
    bool timeout_;
};

// Anything that can answer a chat request: a real model server or a fake.
class Provider {
public:
    virtual ~Provider() = default;
    virtual ChatResponse chat(const ChatRequest& request,
                              std::chrono::milliseconds timeout) = 0;
    virtual std::string name() const = 0;
};

}  // namespace llmgate
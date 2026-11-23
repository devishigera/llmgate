#pragma once

#include <string>

#include "llmgate/provider/provider.h"

namespace llmgate {

struct OllamaOptions {
    std::string name = "ollama";
    std::string url = "http://localhost:11434";  // http://host:port, no path
};

// Talks to an Ollama server over plain HTTP using POST /api/chat with
// "stream": false. Failures are reported as ProviderError:
//
//   could not connect, 5xx, 408, 429   -> transient
//   timed out                          -> transient, timeout
//   other 4xx (bad model, bad request) -> permanent
//   answer we cannot understand        -> permanent
class OllamaProvider : public Provider {
public:
    explicit OllamaProvider(OllamaOptions options = {});

    ChatResponse chat(const ChatRequest& request,
                      std::chrono::milliseconds timeout) override;
    std::string name() const override { return options_.name; }

private:
    OllamaOptions options_;
};

}  // namespace llmgate

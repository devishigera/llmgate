#pragma once

#include <map>
#include <string>

namespace llmgate {

// Our own small request/response types. Handlers only use these, never
// httplib directly, so the same handlers can later run on the epoll server.
// Header names are stored in lower case ("content-type") so lookups are simple.
struct HttpRequest {
    std::string method;  // "GET", "POST", ...
    std::string path;    // "/v1/chat" (no query string)
    std::string body;
    std::map<std::string, std::string> headers;

    // Returns the header value, or "" if it is missing. `name` may be any case.
    std::string header(const std::string& name) const;
};

struct HttpResponse {
    int status = 200;
    std::string body;
    std::map<std::string, std::string> headers;
};

std::string to_lower(const std::string& text);

// {"error":{"code":"...","message":"..."}} with Content-Type application/json.
// Every error in LLMGate uses this one format.
HttpResponse error_response(int status, const std::string& code, const std::string& message);

// A 200-style response with a JSON body.
HttpResponse json_response(int status, const std::string& json_text);

}  // namespace llmgate
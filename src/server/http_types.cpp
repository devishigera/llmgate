#include "llmgate/server/http_types.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace llmgate {

std::string to_lower(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string HttpRequest::header(const std::string& name) const {
    auto it = headers.find(to_lower(name));
    return it == headers.end() ? std::string() : it->second;
}

HttpResponse json_response(int status, const std::string& json_text) {
    HttpResponse r;
    r.status = status;
    r.body = json_text;
    r.headers["content-type"] = "application/json";
    return r;
}

HttpResponse error_response(int status, const std::string& code, const std::string& message) {
    nlohmann::json body = {{"error", {{"code", code}, {"message", message}}}};
    return json_response(status, body.dump());
}

}  // namespace llmgate
#pragma once

#include <functional>
#include <map>
#include <string>

#include "llmgate/server/http_types.h"

namespace llmgate {

using Handler = std::function<HttpResponse(const HttpRequest&)>;

// Maps (method, path) to a handler.
//   unknown path                  -> 404 not_found
//   known path, wrong method      -> 405 method_not_allowed (+ "allow" header)
//   a handler that throws         -> 500 internal_error (the details are not leaked)
class Router {
public:
    void add(const std::string& method, const std::string& path, Handler handler);
    HttpResponse dispatch(const HttpRequest& request) const;

private:
    // path -> (method -> handler)
    std::map<std::string, std::map<std::string, Handler>> routes_;
};

}  // namespace llmgate
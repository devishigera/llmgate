#include "llmgate/server/router.h"

#include <exception>

namespace llmgate {

void Router::add(const std::string& method, const std::string& path, Handler handler) {
    routes_[path][method] = std::move(handler);
}

HttpResponse Router::dispatch(const HttpRequest& request) const {
    auto path_it = routes_.find(request.path);
    if (path_it == routes_.end()) {
        return error_response(404, "not_found", "no such path: " + request.path);
    }

    const auto& methods = path_it->second;
    auto method_it = methods.find(request.method);
    if (method_it == methods.end()) {
        std::string allowed;
        for (const auto& entry : methods) {
            if (!allowed.empty()) allowed += ", ";
            allowed += entry.first;
        }
        HttpResponse r = error_response(405, "method_not_allowed",
                                        request.method + " is not allowed here");
        r.headers["allow"] = allowed;
        return r;
    }

    try {
        return method_it->second(request);
    } catch (const std::exception&) {
        return error_response(500, "internal_error", "something went wrong");
    } catch (...) {
        return error_response(500, "internal_error", "something went wrong");
    }
}

}  // namespace llmgate
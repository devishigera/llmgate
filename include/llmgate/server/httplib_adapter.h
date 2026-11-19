#pragma once

#include <httplib.h>

#include <cstddef>

#include "llmgate/server/router.h"

namespace llmgate {

// Connects an httplib server to our Router. httplib only sees plain
// requests; every decision is made by the Router and the handlers.
// Bodies larger than `max_body_bytes` are refused by httplib with 413.
// The router must outlive the server.
inline void install_router(httplib::Server& server, const Router& router,
                           std::size_t max_body_bytes) {
    server.set_payload_max_length(max_body_bytes);
    server.set_payload_max_length(max_body_bytes);
    server.set_tcp_nodelay(true);
    auto handle = [&router](const httplib::Request& req, httplib::Response& res) {
        HttpRequest request;
        request.method = req.method;
        request.path = req.path;
        request.body = req.body;
        for (const auto& header : req.headers) {
            request.headers[to_lower(header.first)] = header.second;
        }

        const HttpResponse response = router.dispatch(request);

        res.status = response.status;
        std::string content_type = "text/plain";
        for (const auto& header : response.headers) {
            if (header.first == "content-type") {
                content_type = header.second;
            } else {
                res.set_header(header.first, header.second);
            }
        }
        res.set_content(response.body, content_type);
    };

    // Catch every path and method so the Router can answer 404 and 405 itself.
    server.Get(".*", handle);
    server.Post(".*", handle);
    server.Put(".*", handle);
    server.Delete(".*", handle);
    server.Patch(".*", handle);
    server.Options(".*", handle);
}

}  // namespace llmgate
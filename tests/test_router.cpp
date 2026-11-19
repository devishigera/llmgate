#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <stdexcept>

#include "llmgate/server/http_types.h"
#include "llmgate/server/router.h"

using llmgate::HttpRequest;
using llmgate::HttpResponse;
using llmgate::Router;

namespace {

HttpRequest req(const std::string& method, const std::string& path) {
    HttpRequest r;
    r.method = method;
    r.path = path;
    return r;
}

Router make_router() {
    Router router;
    router.add("GET", "/health", [](const HttpRequest&) {
        return llmgate::json_response(200, R"({"status":"ok"})");
    });
    router.add("POST", "/v1/chat", [](const HttpRequest& r) {
        return llmgate::json_response(200, "{\"echo\":" + nlohmann::json(r.body).dump() + "}");
    });
    router.add("GET", "/v1/chat", [](const HttpRequest&) {
        return llmgate::json_response(200, "{}");
    });
    router.add("GET", "/boom", [](const HttpRequest&) -> HttpResponse {
        throw std::runtime_error("secret internal detail");
    });
    return router;
}

}  // namespace

TEST(Router, DispatchesToMatchingHandler) {
    HttpResponse r = make_router().dispatch(req("GET", "/health"));
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, R"({"status":"ok"})");
}

TEST(Router, PassesRequestToHandler) {
    HttpRequest request = req("POST", "/v1/chat");
    request.body = "hello";
    EXPECT_EQ(make_router().dispatch(request).body, R"({"echo":"hello"})");
}

TEST(Router, UnknownPathGives404) {
    HttpResponse r = make_router().dispatch(req("GET", "/nope"));
    EXPECT_EQ(r.status, 404);
    EXPECT_EQ(nlohmann::json::parse(r.body)["error"]["code"], "not_found");
}

TEST(Router, WrongMethodGives405WithAllowHeader) {
    HttpResponse r = make_router().dispatch(req("DELETE", "/v1/chat"));
    EXPECT_EQ(r.status, 405);
    EXPECT_EQ(r.headers.at("allow"), "GET, POST");
    EXPECT_EQ(nlohmann::json::parse(r.body)["error"]["code"], "method_not_allowed");
}

TEST(Router, PathMatchIsExact) {
    EXPECT_EQ(make_router().dispatch(req("GET", "/health/")).status, 404);
    EXPECT_EQ(make_router().dispatch(req("GET", "/HEALTH")).status, 404);
}

TEST(Router, ThrowingHandlerGives500WithoutLeakingDetails) {
    HttpResponse r = make_router().dispatch(req("GET", "/boom"));
    EXPECT_EQ(r.status, 500);
    EXPECT_EQ(r.body.find("secret"), std::string::npos);
    EXPECT_EQ(nlohmann::json::parse(r.body)["error"]["code"], "internal_error");
}

TEST(HttpTypes, HeaderLookupIgnoresCase) {
    HttpRequest r;
    r.headers["authorization"] = "Bearer x";
    EXPECT_EQ(r.header("Authorization"), "Bearer x");
    EXPECT_EQ(r.header("AUTHORIZATION"), "Bearer x");
    EXPECT_EQ(r.header("missing"), "");
}

TEST(HttpTypes, ErrorResponseHasStandardShape) {
    HttpResponse r = llmgate::error_response(429, "rate_limited", "slow down");
    EXPECT_EQ(r.status, 429);
    EXPECT_EQ(r.headers.at("content-type"), "application/json");
    auto j = nlohmann::json::parse(r.body);
    EXPECT_EQ(j["error"]["code"], "rate_limited");
    EXPECT_EQ(j["error"]["message"], "slow down");
}

TEST(HttpTypes, ErrorMessageIsEscapedAsJson) {
    HttpResponse r = llmgate::error_response(400, "bad_request", "has \"quotes\" and \n newline");
    EXPECT_EQ(nlohmann::json::parse(r.body)["error"]["message"], "has \"quotes\" and \n newline");
}

TEST(HttpTypes, ToLower) {
    EXPECT_EQ(llmgate::to_lower("Content-Type"), "content-type");
}
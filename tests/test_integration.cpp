#include <gtest/gtest.h>

#include <httplib.h>

#include <atomic>
#include <memory>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

#include "llmgate/app/app.h"
#include "llmgate/server/httplib_adapter.h"
#include "llmgate/util/sha256.h"

using namespace llmgate;
using json = nlohmann::json;

namespace {

const char* kBody = R"({"model":"mock","messages":[{"role":"user","content":"hello"}]})";

Config test_config(double burst = 1000, double per_second = 1000, std::size_t max_body = 1048576) {
    Config c;
    c.max_body_bytes = max_body;
    KeyConfig k;
    k.team = "claims-team";
    k.key_sha256 = sha256_hex("lg_live_testkey");
    k.rate_limit.burst = burst;
    k.rate_limit.per_second = per_second;
    c.keys.push_back(k);
    ProviderConfig p;
    p.name = "mock";
    p.type = "mock";
    c.providers.push_back(p);
    return c;
}

// Starts a real HTTP server on a free port and stops it at the end of the test.
class TestServer {
public:
    explicit TestServer(const Config& config) : app_(config) {
        install_router(server_, app_.router(), config.max_body_bytes);
        port_ = server_.bind_to_any_port("127.0.0.1");
        EXPECT_GT(port_, 0);
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }
    ~TestServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    int port() const { return port_; }

    httplib::Client client() const {
        httplib::Client c("127.0.0.1", port_);
        c.set_connection_timeout(2);
        c.set_read_timeout(5);
        return c;
    }

    httplib::Result chat(const std::string& body = kBody,
                         const std::string& key = "lg_live_testkey") const {
        httplib::Client c = client();
        httplib::Headers headers;
        if (!key.empty()) headers.emplace("Authorization", "Bearer " + key);
        return c.Post("/v1/chat", headers, body, "application/json");
    }

private:
    App app_;
    httplib::Server server_;
    std::thread thread_;
    int port_ = 0;
};

}  // namespace

TEST(Integration, HealthReturnsOk) {
    TestServer server(test_config());
    auto client = server.client();
    auto res = client.Get("/health");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(json::parse(res->body)["status"], "ok");
    EXPECT_NE(res->get_header_value("Content-Type").find("application/json"), std::string::npos);
}

TEST(Integration, ChatReturnsMockAnswer) {
    TestServer server(test_config());
    auto res = server.chat();
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    json j = json::parse(res->body);
    EXPECT_EQ(j["provider"], "mock");
    EXPECT_EQ(j["cached"], false);
    EXPECT_FALSE(j["content"].get<std::string>().empty());
    EXPECT_GT(j["usage"]["total_tokens"].get<int>(), 0);
}

TEST(Integration, RepeatedPromptIsCached) {
    TestServer server(test_config());
    EXPECT_EQ(json::parse(server.chat()->body)["cached"], false);
    EXPECT_EQ(json::parse(server.chat()->body)["cached"], true);
}

TEST(Integration, InvalidKeyGets401) {
    TestServer server(test_config());
    auto res = server.chat(kBody, "lg_live_wrong");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "unauthorized");
}

TEST(Integration, MissingKeyGets401) {
    TestServer server(test_config());
    auto res = server.chat(kBody, "");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

TEST(Integration, BadJsonGets400) {
    TestServer server(test_config());
    auto res = server.chat("{ nope");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST(Integration, HammeringGets429) {
    TestServer server(test_config(/*burst=*/3, /*per_second=*/0.001));
    int ok = 0, limited = 0;
    std::string retry_after;
    for (int i = 0; i < 10; ++i) {
        auto res = server.chat();
        ASSERT_TRUE(res);
        if (res->status == 200) ++ok;
        if (res->status == 429) {
            ++limited;
            retry_after = res->get_header_value("Retry-After");
        }
    }
    EXPECT_EQ(ok, 3);
    EXPECT_EQ(limited, 7);
    EXPECT_FALSE(retry_after.empty());
}

TEST(Integration, OversizedBodyGets413) {
    TestServer server(test_config(1000, 1000, /*max_body=*/200));
    auto res = server.chat(std::string(5000, 'x'));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 413);

    // The server is still fine afterwards.
    auto ok = server.chat();
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->status, 200);
}

TEST(Integration, UnknownPathGets404) {
    TestServer server(test_config());
    auto client = server.client();
    auto res = client.Get("/nope");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "not_found");
}

TEST(Integration, WrongMethodGets405) {
    TestServer server(test_config());
    auto client = server.client();
    auto res = client.Get("/v1/chat");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 405);
    EXPECT_EQ(res->get_header_value("Allow"), "POST");
}

TEST(Integration, ManyClientsAtOnce) {
    TestServer server(test_config());
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&server, &ok, t] {
            for (int i = 0; i < 25; ++i) {
                const std::string body =
                    std::string(R"({"model":"mock","messages":[{"role":"user","content":"q)") +
                    std::to_string(t) + "-" + std::to_string(i % 5) + R"("}]})";
                auto res = server.chat(body);
                if (res && res->status == 200) ++ok;
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(ok.load(), 200);
}
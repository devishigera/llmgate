#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "llmgate/app/app.h"
#include "llmgate/util/sha256.h"

using namespace llmgate;
using json = nlohmann::json;

namespace {

KeyConfig key() {
    KeyConfig k;
    k.team = "t";
    k.key_sha256 = sha256_hex("lg_live_testkey");
    return k;
}

ProviderConfig provider(const std::string& name, const std::string& type, int priority) {
    ProviderConfig p;
    p.name = name;
    p.type = type;
    p.url = type == "ollama" ? "http://localhost:11434" : "";
    p.priority = priority;
    return p;
}

Config config_with(std::vector<ProviderConfig> providers, std::vector<KeyConfig> keys = {key()}) {
    Config c;
    c.providers = std::move(providers);
    c.keys = std::move(keys);
    return c;
}

HttpRequest chat_request() {
    HttpRequest r;
    r.method = "POST";
    r.path = "/v1/chat";
    r.headers["authorization"] = "Bearer lg_live_testkey";
    r.body = R"({"model":"m","messages":[{"role":"user","content":"hi"}]})";
    return r;
}

bool has_note(const App& app, const std::string& text) {
    for (const auto& n : app.notes()) {
        if (n.find(text) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST(App, HealthWorks) {
    App app(config_with({provider("mock", "mock", 1)}));
    HttpRequest r;
    r.method = "GET";
    r.path = "/health";
    EXPECT_EQ(app.router().dispatch(r).status, 200);
}

TEST(App, ChatWorksWithMockProvider) {
    App app(config_with({provider("mock", "mock", 1)}));
    ASSERT_TRUE(app.chat_enabled());
    HttpResponse r = app.router().dispatch(chat_request());
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(json::parse(r.body)["provider"], "mock");
}

TEST(App, OllamaIsUsedWhenItHasTheLowestPriorityNumber) {
    App app(config_with({provider("ollama", "ollama", 1), provider("fallback", "mock", 2)}));
    ASSERT_TRUE(app.chat_enabled());
    EXPECT_TRUE(has_note(app, "'ollama' (ollama)"));
}

TEST(App, MockIsUsedWhenItHasTheLowestPriorityNumber) {
    App app(config_with({provider("ollama", "ollama", 2), provider("fallback", "mock", 1)}));
    ASSERT_TRUE(app.chat_enabled());
    EXPECT_EQ(json::parse(app.router().dispatch(chat_request()).body)["provider"], "fallback");
}

TEST(App, LowestPriorityNumberWins) {
    App app(config_with({provider("second", "mock", 5), provider("first", "mock", 1)}));
    EXPECT_EQ(json::parse(app.router().dispatch(chat_request()).body)["provider"], "first");
}

TEST(App, NoProvidersDisablesChatButNotHealth) {
    App app(config_with({}));
    EXPECT_FALSE(app.chat_enabled());
    EXPECT_TRUE(has_note(app, "no usable provider"));
    EXPECT_EQ(app.router().dispatch(chat_request()).status, 404);

    HttpRequest h;
    h.method = "GET";
    h.path = "/health";
    EXPECT_EQ(app.router().dispatch(h).status, 200);
}

TEST(App, UnknownProviderTypeDisablesChat) {
    App app(config_with({provider("weird", "carrier-pigeon", 1)}));
    EXPECT_FALSE(app.chat_enabled());
    EXPECT_TRUE(has_note(app, "unknown type"));
}

TEST(App, NoKeysDisablesChat) {
    App app(config_with({provider("mock", "mock", 1)}, {}));
    EXPECT_FALSE(app.chat_enabled());
    EXPECT_TRUE(has_note(app, "no API keys"));
}

TEST(App, DefaultConfigStillServesHealth) {
    App app{Config{}};
    EXPECT_FALSE(app.chat_enabled());
    HttpRequest h;
    h.method = "GET";
    h.path = "/health";
    EXPECT_EQ(app.router().dispatch(h).status, 200);
}

#include <gtest/gtest.h>

#include <chrono>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

#include "fake_ollama.h"
#include "llmgate/provider/ollama.h"

using namespace llmgate;
using json = nlohmann::json;
using std::chrono::milliseconds;

namespace {

ChatRequest request(const std::string& text = "hello") {
    ChatRequest r;
    r.model = "llama3.2:1b";
    r.messages.push_back({"user", text});
    r.max_tokens = 64;
    return r;
}

OllamaProvider provider_for(const FakeOllama& fake) {
    OllamaOptions o;
    o.url = fake.url();
    return OllamaProvider(o);
}

const milliseconds kTimeout{2000};

// Calls chat() and returns the ProviderError it throws (fails the test if none).
ProviderError error_from(OllamaProvider& p, const ChatRequest& r = request(),
                         milliseconds timeout = kTimeout) {
    try {
        p.chat(r, timeout);
    } catch (const ProviderError& e) {
        return e;
    }
    ADD_FAILURE() << "expected a ProviderError";
    return ProviderError("none", false);
}

}  // namespace

TEST(OllamaProvider, ParsesSuccessfulReply) {
    FakeOllama fake;
    auto p = provider_for(fake);
    ChatResponse r = p.chat(request(), kTimeout);
    EXPECT_EQ(r.content, "hello from fake ollama");
    EXPECT_EQ(r.provider, "ollama");
    EXPECT_EQ(r.model, "fake");
    EXPECT_EQ(r.usage.prompt_tokens, 11);
    EXPECT_EQ(r.usage.completion_tokens, 7);
    EXPECT_EQ(r.usage.total(), 18);
}

TEST(OllamaProvider, UsesConfiguredName) {
    FakeOllama fake;
    OllamaOptions o;
    o.name = "local-llama";
    o.url = fake.url();
    OllamaProvider p(o);
    EXPECT_EQ(p.name(), "local-llama");
    EXPECT_EQ(p.chat(request(), kTimeout).provider, "local-llama");
}

TEST(OllamaProvider, SendsTheDocumentedRequest) {
    FakeOllama fake;
    auto p = provider_for(fake);
    ChatRequest r;
    r.model = "llama3.2:1b";
    r.messages.push_back({"system", "be brief"});
    r.messages.push_back({"user", "hi"});
    r.max_tokens = 42;
    p.chat(r, kTimeout);

    json sent = json::parse(fake.last_body());
    EXPECT_EQ(sent["model"], "llama3.2:1b");
    EXPECT_EQ(sent["stream"], false);
    EXPECT_EQ(sent["options"]["num_predict"], 42);
    ASSERT_EQ(sent["messages"].size(), 2u);
    EXPECT_EQ(sent["messages"][0]["role"], "system");
    EXPECT_EQ(sent["messages"][0]["content"], "be brief");
    EXPECT_EQ(sent["messages"][1]["role"], "user");
    EXPECT_NE(fake.last_content_type().find("application/json"), std::string::npos);
}

TEST(OllamaProvider, SpecialCharactersSurviveTheTrip) {
    FakeOllama fake;
    auto p = provider_for(fake);
    const std::string tricky = "quote \" backslash \\ newline \n tab \t unicode \xC3\xA9 \xE2\x9C\x93";
    p.chat(request(tricky), kTimeout);
    EXPECT_EQ(json::parse(fake.last_body())["messages"][0]["content"], tricky);
}

TEST(OllamaProvider, MissingTokenCountsAreEstimated) {
    FakeOllama fake;
    fake.reply_body = R"({"message":{"role":"assistant","content":"12345678"},"done":true})";
    auto p = provider_for(fake);
    ChatResponse r = p.chat(request("abcdefgh"), kTimeout);  // 8 chars each -> 2 tokens
    EXPECT_EQ(r.usage.prompt_tokens, 2);
    EXPECT_EQ(r.usage.completion_tokens, 2);
    EXPECT_EQ(r.model, "llama3.2:1b");  // falls back to the requested model
}

TEST(OllamaProvider, ServerErrorsAreTransient) {
    for (int status : {500, 502, 503, 504}) {
        FakeOllama fake;
        fake.status = status;
        fake.reply_body = R"({"error":"overloaded"})";
        auto p = provider_for(fake);
        ProviderError e = error_from(p);
        EXPECT_TRUE(e.transient()) << status;
        EXPECT_FALSE(e.timeout()) << status;
    }
}

TEST(OllamaProvider, RequestTimeoutAndTooManyRequestsAreTransient) {
    for (int status : {408, 429}) {
        FakeOllama fake;
        fake.status = status;
        fake.reply_body = "{}";
        auto p = provider_for(fake);
        EXPECT_TRUE(error_from(p).transient()) << status;
    }
}

TEST(OllamaProvider, BadRequestIsPermanent) {
    FakeOllama fake;
    fake.status = 400;
    fake.reply_body = R"({"error":"invalid request"})";
    auto p = provider_for(fake);
    ProviderError e = error_from(p);
    EXPECT_FALSE(e.transient());
    EXPECT_NE(std::string(e.what()).find("400"), std::string::npos);
}

TEST(OllamaProvider, UnknownModelIsPermanentAndExplains) {
    FakeOllama fake;
    fake.status = 404;
    fake.reply_body = R"({"error":"model 'nope' not found"})";
    auto p = provider_for(fake);
    ProviderError e = error_from(p);
    EXPECT_FALSE(e.transient());
    EXPECT_NE(std::string(e.what()).find("model 'nope' not found"), std::string::npos);
}

TEST(OllamaProvider, UnparsableJsonIsPermanent) {
    FakeOllama fake;
    fake.reply_body = "this is not json";
    auto p = provider_for(fake);
    EXPECT_FALSE(error_from(p).transient());
}

TEST(OllamaProvider, MissingMessageIsPermanent) {
    FakeOllama fake;
    fake.reply_body = R"({"model":"x","done":true})";
    auto p = provider_for(fake);
    EXPECT_FALSE(error_from(p).transient());
}

TEST(OllamaProvider, MissingContentIsPermanent) {
    FakeOllama fake;
    fake.reply_body = R"({"message":{"role":"assistant"},"done":true})";
    auto p = provider_for(fake);
    EXPECT_FALSE(error_from(p).transient());
}

TEST(OllamaProvider, ErrorFieldInA200AnswerIsPermanent) {
    FakeOllama fake;
    fake.reply_body = R"({"error":"something odd"})";
    auto p = provider_for(fake);
    ProviderError e = error_from(p);
    EXPECT_FALSE(e.transient());
    EXPECT_NE(std::string(e.what()).find("something odd"), std::string::npos);
}

TEST(OllamaProvider, NonObjectReplyIsPermanent) {
    FakeOllama fake;
    fake.reply_body = "[1,2,3]";
    auto p = provider_for(fake);
    EXPECT_FALSE(error_from(p).transient());
}

TEST(OllamaProvider, TimesOutOnSlowServer) {
    FakeOllama fake;
    fake.delay_ms = 600;
    auto p = provider_for(fake);
    auto start = std::chrono::steady_clock::now();
    ProviderError e = error_from(p, request(), milliseconds(100));
    auto took = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(e.timeout());
    EXPECT_TRUE(e.transient());
    EXPECT_LT(took, milliseconds(500));  // we did not wait for the slow answer
}

TEST(OllamaProvider, SlowButWithinTimeoutSucceeds) {
    FakeOllama fake;
    fake.delay_ms = 100;
    auto p = provider_for(fake);
    EXPECT_NO_THROW(p.chat(request(), milliseconds(2000)));
}

TEST(OllamaProvider, ConnectionRefusedIsTransientNotTimeout) {
    std::string dead_url;
    {
        FakeOllama fake;
        dead_url = fake.url();
        fake.stop();  // nothing listens on that port any more
    }
    OllamaOptions o;
    o.url = dead_url;
    OllamaProvider p(o);
    ProviderError e = error_from(p);
    EXPECT_TRUE(e.transient());
    EXPECT_FALSE(e.timeout());
    EXPECT_NE(std::string(e.what()).find("cannot reach"), std::string::npos);
}

TEST(OllamaProvider, SafeUnderConcurrentCalls) {
    FakeOllama fake;
    auto p = provider_for(fake);
    std::vector<std::thread> threads;
    std::atomic<int> ok{0};
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 15; ++i) {
                try {
                    p.chat(request(), kTimeout);
                    ++ok;
                } catch (const ProviderError&) {
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(ok.load(), 60);
    EXPECT_EQ(fake.calls(), 60);
}

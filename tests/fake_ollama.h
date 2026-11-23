#pragma once

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

// A tiny fake Ollama server for tests. It listens on a free local port and
// answers POST /api/chat the way a test tells it to.
class FakeOllama {
public:
    // What the next answers look like. Change these between calls if needed.
    int status = 200;
    std::string reply_body =
        R"({"model":"fake","message":{"role":"assistant","content":"hello from fake ollama"},)"
        R"("done":true,"prompt_eval_count":11,"eval_count":7})";
    int delay_ms = 0;

    FakeOllama() {
        server_.Post("/api/chat", [this](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                last_body_ = req.body;
                last_content_type_ = req.get_header_value("Content-Type");
                ++calls_;
            }
            if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            res.status = status;
            res.set_content(reply_body, "application/json");
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~FakeOllama() { stop(); }

    void stop() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    int port() const { return port_; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

    std::string last_body() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_body_;
    }
    std::string last_content_type() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_content_type_;
    }
    int calls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }

private:
    httplib::Server server_;
    std::thread thread_;
    int port_ = 0;
    mutable std::mutex mutex_;
    std::string last_body_;
    std::string last_content_type_;
    int calls_ = 0;
};

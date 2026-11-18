#include <httplib.h>

#include <cstdlib>
#include <iostream>

#include "llmgate/health.h"

int main() {
    const char* port_env = std::getenv("LLMGATE_PORT");
    const int port = port_env ? std::atoi(port_env) : 8080;

    httplib::Server server;

    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(llmgate::health_json(), "application/json");
    });

    std::cout << "LLMGate listening on 0.0.0.0:" << port << std::endl;
    if (!server.listen("0.0.0.0", port)) {
        std::cerr << "Failed to listen on port " << port << std::endl;
        return 1;
    }
    return 0;
}
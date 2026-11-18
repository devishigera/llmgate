#include <httplib.h>

#include <cstdlib>
#include <iostream>
#include <string>

#include "llmgate/config/config.h"
#include "llmgate/health.h"

namespace {

void print_usage(const char* program) {
    std::cerr << "Usage: " << program << " [--config <path>]\n"
              << "  --config <path>   load settings from a JSON file\n"
              << "  --help            show this message\n"
              << "Environment: LLMGATE_PORT overrides the listen port.\n";
}

}  // namespace

int main(int argc, char** argv) {
    // 1. Command-line arguments
    std::string config_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--config") {
            if (i + 1 >= argc) {
                std::cerr << "error: --config needs a path\n";
                return 2;
            }
            config_path = argv[++i];
        } else if (arg.rfind("--config=", 0) == 0) {
            config_path = arg.substr(std::string("--config=").size());
        } else {
            std::cerr << "error: unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    // 2. Load the configuration (defaults if no file was given)
    llmgate::Config config;
    if (!config_path.empty()) {
        try {
            config = llmgate::load_config_from_file(config_path);
        } catch (const llmgate::ConfigError& e) {
            std::cerr << e.what() << "\n";
            return 1;
        }
    }

    // 3. LLMGATE_PORT overrides the port from the file
    if (const char* env_port = std::getenv("LLMGATE_PORT")) {
        int port = 0;
        if (!llmgate::parse_port(env_port, port)) {
            std::cerr << "error: LLMGATE_PORT must be a number from 1 to 65535\n";
            return 2;
        }
        config.listen_port = port;
    }

    std::cout << llmgate::describe(config);

    // 4. Start the server
    httplib::Server server;
    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(llmgate::health_json(), "application/json");
    });

    std::cout << "LLMGate listening on " << config.listen_host << ":"
              << config.listen_port << std::endl;
    if (!server.listen(config.listen_host, config.listen_port)) {
        std::cerr << "Failed to listen on " << config.listen_host << ":"
                  << config.listen_port << std::endl;
        return 1;
    }
    return 0;
}
#include <httplib.h>

#include <cstdlib>
#include <iostream>
#include <string>

#include "llmgate/app/app.h"
#include "llmgate/config/config.h"
#include "llmgate/server/httplib_adapter.h"
#include "llmgate/util/logger.h"

namespace {

void print_usage(const char* program) {
    std::cerr << "Usage: " << program << " [--config <path>]\n"
              << "  --config <path>   load settings from a JSON file\n"
              << "  --help            show this message\n"
              << "Environment: LLMGATE_PORT overrides the listen port.\n"
              << "             LLMGATE_LOG_LEVEL is debug, info (default), warn, error or off.\n"
              << "Request logs are JSON lines on stdout; startup messages go to stderr.\n";
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
    } else {
        std::cerr << "note: no --config given, only /health will work\n";
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

    // 4. Log level (LLMGATE_LOG_LEVEL). Request logs are JSON lines on stdout.
    llmgate::LogLevel log_level = llmgate::LogLevel::Info;
    if (const char* env_level = std::getenv("LLMGATE_LOG_LEVEL")) {
        if (!llmgate::parse_log_level(env_level, log_level)) {
            std::cerr << "error: LLMGATE_LOG_LEVEL must be debug, info, warn, error or off\n";
            return 2;
        }
    }
    llmgate::Logger logger(std::cout, log_level);

    // Human-readable startup messages go to stderr, so stdout holds only JSON lines.
    std::cerr << llmgate::describe(config);

    // 5. Build the gateway and connect it to the HTTP server
    llmgate::App app(config, llmgate::real_clock(), llmgate::real_wall_clock(),
                     llmgate::real_sleeper(), &logger);
    for (const auto& note : app.notes()) std::cerr << "startup: " << note << "\n";

    httplib::Server server;
    llmgate::install_router(server, app.router(), config.max_body_bytes);

    std::cerr << "LLMGate listening on " << config.listen_host << ":" << config.listen_port
              << std::endl;
    logger.info("listening", {{"host", config.listen_host}, {"port", config.listen_port}});
    if (!server.listen(config.listen_host, config.listen_port)) {
        std::cerr << "Failed to listen on " << config.listen_host << ":"
                  << config.listen_port << std::endl;
        return 1;
    }
    return 0;
}

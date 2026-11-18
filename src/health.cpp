#include "llmgate/health.h"

#include <nlohmann/json.hpp>

namespace llmgate {

std::string health_json() {
    nlohmann::json j;
    j["status"] = "ok";
    return j.dump();
}

}  // namespace llmgate
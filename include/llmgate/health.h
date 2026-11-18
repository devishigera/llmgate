#pragma once

#include <string>

namespace llmgate {

// Returns the JSON body for GET /health, e.g. {"status":"ok"}
std::string health_json();

}  // namespace llmgate
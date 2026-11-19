#pragma once

#include <string>

namespace llmgate {

// SHA-256 of `data`, returned as 64 lowercase hexadecimal characters.
std::string sha256_hex(const std::string& data);

}  // namespace llmgate
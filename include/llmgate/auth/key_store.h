#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "llmgate/config/config.h"

namespace llmgate {

// Extracts the token from an "Authorization" header value.
// Accepts exactly "Bearer <token>": case-sensitive scheme, one space, and a
// non-empty token with no spaces. Anything else gives std::nullopt.
std::optional<std::string> parse_bearer(const std::string& authorization_header);

// Holds the configured keys, indexed by the SHA-256 of the key. The raw keys
// are never stored; clients send the raw key and we hash it to look it up.
class KeyStore {
public:
    explicit KeyStore(const std::vector<KeyConfig>& keys);

    // Returns the matching key's settings, or nullptr if the key is unknown.
    // The pointer stays valid for the lifetime of the KeyStore.
    const KeyConfig* authenticate(const std::string& raw_key) const;

    std::size_t size() const { return by_hash_.size(); }

private:
    std::unordered_map<std::string, KeyConfig> by_hash_;
};

}  // namespace llmgate
#include "llmgate/auth/key_store.h"

#include <openssl/crypto.h>

#include "llmgate/util/sha256.h"

namespace llmgate {

std::optional<std::string> parse_bearer(const std::string& header) {
    static const std::string kPrefix = "Bearer ";
    if (header.size() <= kPrefix.size()) return std::nullopt;
    if (header.compare(0, kPrefix.size(), kPrefix) != 0) return std::nullopt;

    std::string token = header.substr(kPrefix.size());
    if (token.empty() || token.find(' ') != std::string::npos) return std::nullopt;
    return token;
}

KeyStore::KeyStore(const std::vector<KeyConfig>& keys) {
    for (const auto& key : keys) {
        by_hash_.emplace(key.key_sha256, key);
    }
}

const KeyConfig* KeyStore::authenticate(const std::string& raw_key) const {
    if (raw_key.empty()) return nullptr;

    const std::string hash = sha256_hex(raw_key);
    auto it = by_hash_.find(hash);
    if (it == by_hash_.end()) return nullptr;

    // Confirm with a constant-time comparison so timing never reveals how
    // many leading characters matched.
    const std::string& stored = it->second.key_sha256;
    if (stored.size() != hash.size() ||
        CRYPTO_memcmp(stored.data(), hash.data(), hash.size()) != 0) {
        return nullptr;
    }
    return &it->second;
}

}  // namespace llmgate
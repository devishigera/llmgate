#include "llmgate/util/sha256.h"

#include <openssl/evp.h>

#include <memory>
#include <stdexcept>

namespace llmgate {

std::string sha256_hex(const std::string& data) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),
                                                                &EVP_MD_CTX_free);
    if (!ctx) throw std::runtime_error("sha256: cannot create context");

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx.get(), data.data(), data.size()) != 1 ||
        EVP_DigestFinal_ex(ctx.get(), digest, &length) != 1) {
        throw std::runtime_error("sha256: OpenSSL digest failed");
    }

    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (unsigned int i = 0; i < length; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }
    return out;
}

}  // namespace llmgate
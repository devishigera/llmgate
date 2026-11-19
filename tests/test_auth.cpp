#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "llmgate/auth/key_store.h"
#include "llmgate/util/sha256.h"

using llmgate::KeyConfig;
using llmgate::KeyStore;
using llmgate::parse_bearer;
using llmgate::sha256_hex;

namespace {

KeyStore make_store() {
    KeyConfig claims;
    claims.team = "claims-team";
    claims.key_sha256 = sha256_hex("lg_live_testkey");

    KeyConfig fraud;
    fraud.team = "fraud-team";
    fraud.key_sha256 = sha256_hex("lg_live_otherkey");

    return KeyStore({claims, fraud});
}

}  // namespace

TEST(Sha256, KnownAnswerForAbc) {
    EXPECT_EQ(sha256_hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256, KnownAnswerForEmptyString) {
    EXPECT_EQ(sha256_hex(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256, MatchesExampleConfigHash) {
    EXPECT_EQ(sha256_hex("lg_live_testkey"),
              "456acec14fd9a742e5306de8bc0aaaeaf57d506f7d0bf46b44326cfbbe6fcea0");
}

TEST(Sha256, AlwaysSixtyFourLowercaseHexChars) {
    const std::string h = sha256_hex(std::string(1000, 'x'));
    EXPECT_EQ(h.size(), 64u);
    for (char c : h) EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
}

TEST(KeyStore, ValidKeyAuthenticates) {
    KeyStore store = make_store();
    const KeyConfig* k = store.authenticate("lg_live_testkey");
    ASSERT_NE(k, nullptr);
    EXPECT_EQ(k->team, "claims-team");
}

TEST(KeyStore, EachKeyMapsToItsOwnTeam) {
    KeyStore store = make_store();
    const KeyConfig* k = store.authenticate("lg_live_otherkey");
    ASSERT_NE(k, nullptr);
    EXPECT_EQ(k->team, "fraud-team");
}

TEST(KeyStore, WrongKeyRejected) {
    EXPECT_EQ(make_store().authenticate("lg_live_wrong"), nullptr);
}

TEST(KeyStore, AlmostRightKeyRejected) {
    EXPECT_EQ(make_store().authenticate("lg_live_testkey "), nullptr);
    EXPECT_EQ(make_store().authenticate("LG_LIVE_TESTKEY"), nullptr);
}

TEST(KeyStore, EmptyKeyRejected) {
    EXPECT_EQ(make_store().authenticate(""), nullptr);
}

TEST(KeyStore, StoredHashIsNotAValidKey) {
    // Someone who steals the config file must not be able to log in with it.
    EXPECT_EQ(make_store().authenticate(sha256_hex("lg_live_testkey")), nullptr);
}

TEST(KeyStore, SizeCountsKeys) {
    EXPECT_EQ(make_store().size(), 2u);
}

TEST(ParseBearer, AcceptsValidHeader) {
    auto token = parse_bearer("Bearer lg_live_testkey");
    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(*token, "lg_live_testkey");
}

TEST(ParseBearer, RejectsMissingScheme) {
    EXPECT_FALSE(parse_bearer("lg_live_testkey").has_value());
    EXPECT_FALSE(parse_bearer("").has_value());
    EXPECT_FALSE(parse_bearer("Basic abc").has_value());
}

TEST(ParseBearer, RejectsWrongCaseScheme) {
    EXPECT_FALSE(parse_bearer("bearer abc").has_value());
    EXPECT_FALSE(parse_bearer("BEARER abc").has_value());
}

TEST(ParseBearer, RejectsExtraSpaces) {
    EXPECT_FALSE(parse_bearer("Bearer  abc").has_value());
    EXPECT_FALSE(parse_bearer("Bearer abc ").has_value());
    EXPECT_FALSE(parse_bearer("Bearer a b").has_value());
}

TEST(ParseBearer, RejectsEmptyToken) {
    EXPECT_FALSE(parse_bearer("Bearer").has_value());
    EXPECT_FALSE(parse_bearer("Bearer ").has_value());
}
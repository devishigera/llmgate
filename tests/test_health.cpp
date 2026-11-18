#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "llmgate/health.h"

TEST(Health, ReturnsOkStatus) {
    auto body = nlohmann::json::parse(llmgate::health_json());
    EXPECT_EQ(body.at("status"), "ok");
}
cmake_minimum_required(VERSION 3.20)
project(llmgate VERSION 0.1.0 LANGUAGES CXX)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

include(FetchContent)
FetchContent_Declare(httplib GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git GIT_TAG v0.18.3)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_Declare(googletest URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib json googletest)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(llmgate_core
  src/health.cpp
  src/limiter/token_bucket.cpp
  src/config/config.cpp
  src/util/sha256.cpp
  src/auth/key_store.cpp
  src/provider/mock.cpp
  src/resilience/circuit_breaker.cpp
  src/server/http_types.cpp
  src/server/router.cpp
  src/handlers/chat_handler.cpp
  src/app/app.cpp
)
target_include_directories(llmgate_core PUBLIC include)
target_compile_options(llmgate_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(llmgate_core PUBLIC nlohmann_json::nlohmann_json OpenSSL::Crypto)

add_executable(llmgate src/main.cpp)
target_compile_options(llmgate PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(llmgate PRIVATE llmgate_core httplib::httplib Threads::Threads)

enable_testing()
add_executable(llmgate_tests
  tests/test_health.cpp
  tests/test_token_bucket.cpp
  tests/test_lru_cache.cpp
  tests/test_config.cpp
  tests/test_auth.cpp
  tests/test_mock_provider.cpp
  tests/test_circuit_breaker.cpp
  tests/test_router.cpp
  tests/test_chat_handler.cpp
  tests/test_app.cpp
  tests/test_integration.cpp
)
target_compile_definitions(llmgate_tests PRIVATE LLMGATE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(llmgate_tests PRIVATE llmgate_core httplib::httplib GTest::gtest_main Threads::Threads)

include(GoogleTest)
gtest_discover_tests(llmgate_tests)
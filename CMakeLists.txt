cmake_minimum_required(VERSION 3.21)

project(hl_maker_lighter_arb
    VERSION 0.1.0
    DESCRIPTION "HL maker / Lighter taker hedger"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(arb_core
    src/crypto.cpp
    src/engine.cpp
    src/http.cpp
    src/msgpack.cpp
    src/native_trading.cpp
    src/public_exchange.cpp
    src/strategy.cpp
    src/risk.cpp
)

target_include_directories(arb_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(arb_core PUBLIC cxx_std_20)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(arb_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

find_library(CURL_LIB curl REQUIRED)
find_library(DL_LIB dl)
find_library(SECP256K1_LIB secp256k1 PATHS /opt/homebrew/lib REQUIRED)
target_include_directories(arb_core PUBLIC /opt/homebrew/include)
target_link_libraries(arb_core PUBLIC ${CURL_LIB} ${SECP256K1_LIB})
if (DL_LIB)
    target_link_libraries(arb_core PUBLIC ${DL_LIB})
endif()

# --- WebSocket + Market Feed library (Boost.Beast + OpenSSL) ---
find_package(Boost 1.80 REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(arb_ws
    src/ws_client.cpp
    src/market_feed.cpp
)

target_include_directories(arb_ws PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(arb_ws PUBLIC arb_core Boost::headers OpenSSL::SSL OpenSSL::Crypto)
target_compile_features(arb_ws PUBLIC cxx_std_20)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(arb_ws PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

add_executable(arb_demo
    src/main.cpp
)

target_link_libraries(arb_demo PRIVATE arb_core)

add_executable(arb_tests
    tests/test_strategy.cpp
)

target_link_libraries(arb_tests PRIVATE arb_core)

add_executable(native_baseline
    src/native_baseline.cpp
)

target_link_libraries(native_baseline PRIVATE arb_core)

add_executable(native_smoke
    src/native_smoke.cpp
)

target_link_libraries(native_smoke PRIVATE arb_core)

add_executable(arb_live
    src/main_live.cpp
)

target_link_libraries(arb_live PRIVATE arb_ws)

add_executable(ws_smoke
    src/ws_smoke.cpp
)

target_link_libraries(ws_smoke PRIVATE arb_ws)

add_executable(risk_tests
    tests/test_risk.cpp
)

target_link_libraries(risk_tests PRIVATE arb_core)

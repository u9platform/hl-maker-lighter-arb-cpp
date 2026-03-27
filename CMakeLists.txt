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
    src/bridge_exchange.cpp
    src/engine.cpp
    src/http.cpp
    src/process.cpp
    src/public_exchange.cpp
    src/strategy.cpp
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
target_link_libraries(arb_core PUBLIC ${CURL_LIB})

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

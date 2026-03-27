#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace arb {

using Bytes32 = std::array<std::uint8_t, 32>;

[[nodiscard]] Bytes32 keccak256(const std::vector<std::uint8_t>& data);
[[nodiscard]] Bytes32 keccak256(const std::string& data);
[[nodiscard]] std::vector<std::uint8_t> hex_to_bytes(std::string hex);
[[nodiscard]] std::string bytes_to_hex(const std::uint8_t* data, std::size_t size, bool with_prefix = true);
[[nodiscard]] std::string bytes32_to_hex(const Bytes32& value, bool with_prefix = true);

}  // namespace arb

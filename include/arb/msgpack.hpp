#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace arb {

using ByteBuffer = std::vector<std::uint8_t>;

void mp_pack_str(ByteBuffer& out, const std::string& value);
void mp_pack_bool(ByteBuffer& out, bool value);
void mp_pack_uint(ByteBuffer& out, std::uint64_t value);
void mp_pack_array_size(ByteBuffer& out, std::uint32_t size);
void mp_pack_map_size(ByteBuffer& out, std::uint32_t size);

}  // namespace arb

#include "arb/msgpack.hpp"

namespace arb {

void mp_pack_str(ByteBuffer& out, const std::string& value) {
    const auto size = static_cast<std::uint32_t>(value.size());
    if (size <= 31) {
        out.push_back(static_cast<std::uint8_t>(0xa0U | size));
    } else {
        out.push_back(0xd9);
        out.push_back(static_cast<std::uint8_t>(size));
    }
    out.insert(out.end(), value.begin(), value.end());
}

void mp_pack_bool(ByteBuffer& out, bool value) {
    out.push_back(value ? 0xc3 : 0xc2);
}

void mp_pack_uint(ByteBuffer& out, std::uint64_t value) {
    if (value <= 0x7f) {
        out.push_back(static_cast<std::uint8_t>(value));
    } else if (value <= 0xff) {
        out.push_back(0xcc);
        out.push_back(static_cast<std::uint8_t>(value));
    } else if (value <= 0xffff) {
        out.push_back(0xcd);
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(value & 0xff));
    } else if (value <= 0xffffffffULL) {
        out.push_back(0xce);
        for (int shift = 24; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
        }
    } else {
        out.push_back(0xcf);
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
        }
    }
}

void mp_pack_array_size(ByteBuffer& out, std::uint32_t size) {
    if (size <= 15) {
        out.push_back(static_cast<std::uint8_t>(0x90U | size));
    } else {
        out.push_back(0xdc);
        out.push_back(static_cast<std::uint8_t>((size >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(size & 0xff));
    }
}

void mp_pack_map_size(ByteBuffer& out, std::uint32_t size) {
    if (size <= 15) {
        out.push_back(static_cast<std::uint8_t>(0x80U | size));
    } else {
        out.push_back(0xde);
        out.push_back(static_cast<std::uint8_t>((size >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(size & 0xff));
    }
}

}  // namespace arb

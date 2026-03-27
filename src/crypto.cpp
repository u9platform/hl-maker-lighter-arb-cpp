#include "arb/crypto.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace arb {

namespace {

constexpr std::uint64_t kRoundConstants[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
};

constexpr int kRotation[5][5] = {
    {0, 36, 3, 41, 18},
    {1, 44, 10, 45, 2},
    {62, 6, 43, 15, 61},
    {28, 55, 25, 21, 56},
    {27, 20, 39, 8, 14},
};

std::uint64_t rotl64(std::uint64_t x, int shift) {
    return (x << shift) | (x >> (64 - shift));
}

void keccakf(std::uint64_t state[25]) {
    for (std::uint64_t rc : kRoundConstants) {
        std::uint64_t c[5];
        for (int x = 0; x < 5; ++x) {
            c[x] = state[x] ^ state[x + 5] ^ state[x + 10] ^ state[x + 15] ^ state[x + 20];
        }

        std::uint64_t d[5];
        for (int x = 0; x < 5; ++x) {
            d[x] = c[(x + 4) % 5] ^ rotl64(c[(x + 1) % 5], 1);
        }
        for (int x = 0; x < 5; ++x) {
            for (int y = 0; y < 5; ++y) {
                state[x + 5 * y] ^= d[x];
            }
        }

        std::uint64_t b[25];
        for (int x = 0; x < 5; ++x) {
            for (int y = 0; y < 5; ++y) {
                b[y + 5 * ((2 * x + 3 * y) % 5)] = rotl64(state[x + 5 * y], kRotation[x][y]);
            }
        }

        for (int x = 0; x < 5; ++x) {
            for (int y = 0; y < 5; ++y) {
                state[x + 5 * y] = b[x + 5 * y] ^ ((~b[((x + 1) % 5) + 5 * y]) & b[((x + 2) % 5) + 5 * y]);
            }
        }

        state[0] ^= rc;
    }
}

}  // namespace

Bytes32 keccak256(const std::vector<std::uint8_t>& data) {
    constexpr std::size_t rate = 136;
    std::uint64_t state[25] = {};

    std::size_t offset = 0;
    while (offset + rate <= data.size()) {
        for (std::size_t i = 0; i < rate / 8; ++i) {
            std::uint64_t lane = 0;
            for (int j = 0; j < 8; ++j) {
                lane |= static_cast<std::uint64_t>(data[offset + i * 8 + j]) << (8 * j);
            }
            state[i] ^= lane;
        }
        keccakf(state);
        offset += rate;
    }

    std::uint8_t block[rate];
    std::fill(std::begin(block), std::end(block), 0);
    const std::size_t remaining = data.size() - offset;
    for (std::size_t i = 0; i < remaining; ++i) {
        block[i] = data[offset + i];
    }
    block[remaining] = 0x01;
    block[rate - 1] ^= 0x80;

    for (std::size_t i = 0; i < rate / 8; ++i) {
        std::uint64_t lane = 0;
        for (int j = 0; j < 8; ++j) {
            lane |= static_cast<std::uint64_t>(block[i * 8 + j]) << (8 * j);
        }
        state[i] ^= lane;
    }
    keccakf(state);

    Bytes32 output {};
    for (std::size_t i = 0; i < output.size(); ++i) {
        output[i] = static_cast<std::uint8_t>((state[i / 8] >> (8 * (i % 8))) & 0xff);
    }
    return output;
}

Bytes32 keccak256(const std::string& data) {
    return keccak256(std::vector<std::uint8_t>(data.begin(), data.end()));
}

std::vector<std::uint8_t> hex_to_bytes(std::string hex) {
    if (hex.rfind("0x", 0) == 0 || hex.rfind("0X", 0) == 0) {
        hex = hex.substr(2);
    }
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("hex string must have even length");
    }
    std::vector<std::uint8_t> out(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    }
    return out;
}

std::string bytes_to_hex(const std::uint8_t* data, std::size_t size, bool with_prefix) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2 + (with_prefix ? 2 : 0));
    if (with_prefix) {
        out += "0x";
    }
    for (std::size_t i = 0; i < size; ++i) {
        out += digits[(data[i] >> 4) & 0x0f];
        out += digits[data[i] & 0x0f];
    }
    return out;
}

std::string bytes32_to_hex(const Bytes32& value, bool with_prefix) {
    return bytes_to_hex(value.data(), value.size(), with_prefix);
}

}  // namespace arb

#include "arb/crypto.hpp"
#include "arb/msgpack.hpp"

#include <iostream>
#include <iomanip>
#include <cstdint>
#include <vector>
#include <sstream>

#include <secp256k1.h>
#include <secp256k1_recovery.h>

using Bytes32 = arb::Bytes32;
using ByteBuffer = std::vector<std::uint8_t>;

std::string to_hex(const std::uint8_t* data, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; ++i) {
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(data[i]);
    }
    return oss.str();
}

std::string to_hex(const Bytes32& b) { return to_hex(b.data(), b.size()); }
std::string to_hex(const ByteBuffer& b) { return to_hex(b.data(), b.size()); }

Bytes32 uint256_word(std::uint64_t value) {
    Bytes32 out {};
    for (int i = 0; i < 8; ++i) {
        out[31 - i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xff);
    }
    return out;
}

void append_word(ByteBuffer& out, const Bytes32& word) {
    out.insert(out.end(), word.begin(), word.end());
}

int main() {
    // Build the same msgpack action
    ByteBuffer mp;
    arb::mp_pack_map_size(mp, 3);
    arb::mp_pack_str(mp, "type");
    arb::mp_pack_str(mp, "order");
    arb::mp_pack_str(mp, "orders");
    arb::mp_pack_array_size(mp, 1);
    arb::mp_pack_map_size(mp, 6);
    arb::mp_pack_str(mp, "a");
    arb::mp_pack_uint(mp, 132);
    arb::mp_pack_str(mp, "b");
    arb::mp_pack_bool(mp, true);
    arb::mp_pack_str(mp, "p");
    arb::mp_pack_str(mp, "37.9");
    arb::mp_pack_str(mp, "s");
    arb::mp_pack_str(mp, "0.26");
    arb::mp_pack_str(mp, "r");
    arb::mp_pack_bool(mp, false);
    arb::mp_pack_str(mp, "t");
    arb::mp_pack_map_size(mp, 1);
    arb::mp_pack_str(mp, "limit");
    arb::mp_pack_map_size(mp, 1);
    arb::mp_pack_str(mp, "tif");
    arb::mp_pack_str(mp, "Alo");
    arb::mp_pack_str(mp, "grouping");
    arb::mp_pack_str(mp, "na");

    std::cerr << "Msgpack hex: " << to_hex(mp) << "\n";
    std::cerr << "Expected:    83a474797065a56f72646572a66f72646572739186a161cc84a162c3a170a433372e39a173a4302e3236a172c2a17481a56c696d697481a3746966a3416c6fa867726f7570696e67a26e61\n";

    // Append nonce (big-endian 8 bytes) = 1711584000000
    std::uint64_t nonce = 1711584000000ULL;
    for (int i = 7; i >= 0; --i) {
        mp.push_back(static_cast<std::uint8_t>((nonce >> (8 * i)) & 0xff));
    }

    // Append vault flag + address
    mp.push_back(0x01);
    // 0xa3fb9fed3fe24baaafcbe783409bedaeff28d0a7
    const auto vault_bytes = arb::hex_to_bytes("0xa3fb9fed3fe24baaafcbe783409bedaeff28d0a7");
    mp.insert(mp.end(), vault_bytes.begin(), vault_bytes.end());
    std::cerr << "Vault bytes hex: " << to_hex(vault_bytes.data(), vault_bytes.size()) << "\n";

    Bytes32 action_hash = arb::keccak256(mp);
    std::cerr << "Action hash: " << to_hex(action_hash) << "\n";
    std::cerr << "Expected:    4e6629ab0b544a07d7c5c249cbbff666ccac2141bf4d7f4abb51fe610ec77ae3\n";

    // Now do EIP712 signing
    const Bytes32 domain_typehash = arb::keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)");
    const Bytes32 name_hash = arb::keccak256("Exchange");
    const Bytes32 version_hash = arb::keccak256("1");
    ByteBuffer domain_data;
    append_word(domain_data, domain_typehash);
    append_word(domain_data, name_hash);
    append_word(domain_data, version_hash);
    append_word(domain_data, uint256_word(1337));
    Bytes32 zero_addr {};
    append_word(domain_data, zero_addr);
    const Bytes32 domain_separator = arb::keccak256(domain_data);
    std::cerr << "Domain separator: " << to_hex(domain_separator) << "\n";
    std::cerr << "Expected:         d79297fcdf2ffcd4ae223d01edaa2ba214ff8f401d7c9300d995d17c82aa4040\n";

    const Bytes32 agent_typehash = arb::keccak256("Agent(string source,bytes32 connectionId)");
    const Bytes32 source_hash = arb::keccak256("a");
    ByteBuffer msg_data;
    append_word(msg_data, agent_typehash);
    append_word(msg_data, source_hash);
    append_word(msg_data, action_hash);
    const Bytes32 message_hash = arb::keccak256(msg_data);
    std::cerr << "Message hash: " << to_hex(message_hash) << "\n";
    std::cerr << "Expected:     5e211e9e472f1e76b8f6b2bc857f01e5f897ce997082c7e24b6fb691c0dda242\n";

    ByteBuffer digest_bytes;
    digest_bytes.push_back(0x19);
    digest_bytes.push_back(0x01);
    digest_bytes.insert(digest_bytes.end(), domain_separator.begin(), domain_separator.end());
    digest_bytes.insert(digest_bytes.end(), message_hash.begin(), message_hash.end());
    const Bytes32 digest = arb::keccak256(digest_bytes);
    std::cerr << "Digest: " << to_hex(digest) << "\n";
    std::cerr << "Expected: 747dff9b39613c0689a1a852a0991a86d61181c79febcc9340e94865b936451d\n";

    // Sign with private key
    const auto priv = arb::hex_to_bytes("0x83fee78b849ead73998d2833ea80d405c5cbd3516d7e5883461248af6e47afac");
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    secp256k1_ecdsa_recoverable_signature sig;
    if (secp256k1_ecdsa_sign_recoverable(ctx, &sig, digest.data(), priv.data(), nullptr, nullptr) != 1) {
        std::cerr << "SIGN FAILED\n";
        return 1;
    }
    unsigned char compact[64];
    int recid = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(ctx, compact, &recid, &sig);
    secp256k1_context_destroy(ctx);

    std::cerr << "r: 0x" << to_hex(compact, 32) << "\n";
    std::cerr << "s: 0x" << to_hex(compact + 32, 32) << "\n";
    std::cerr << "v: " << (27 + recid) << "\n";
    std::cerr << "Expected r: 0x9578274e307b49d6bb45e8ecc840bcd3fc687507f5a4152b9b8cce9a2b48dd36\n";
    std::cerr << "Expected s: 0x7b4564359d08ea00e6ac9bfd8a05e89e7c940a43fba3a447463e0c30ca71df72\n";
    std::cerr << "Expected v: 28\n";

    return 0;
}

#include "arb/native_trading.hpp"

#include "arb/http.hpp"
#include "arb/msgpack.hpp"

#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include <dlfcn.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace arb {

namespace {

double first_match_as_double(const std::string& text, const std::regex& pattern) {
    std::smatch match;
    if (!std::regex_search(text, match, pattern)) {
        throw std::runtime_error("failed to parse numeric field");
    }
    return std::stod(match[1].str());
}

std::string first_match_as_string(const std::string& text, const std::regex& pattern) {
    std::smatch match;
    if (!std::regex_search(text, match, pattern)) {
        throw std::runtime_error("failed to parse string field");
    }
    return match[1].str();
}

std::uint64_t current_timestamp_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

void append_word(std::vector<std::uint8_t>& out, const Bytes32& word) {
    out.insert(out.end(), word.begin(), word.end());
}

Bytes32 uint256_word(std::uint64_t value) {
    Bytes32 out {};
    for (int i = 0; i < 8; ++i) {
        out[31 - i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xff);
    }
    return out;
}

Bytes32 address_word() {
    return {};
}

std::string trim_0x(const std::string& hex) {
    if (hex.rfind("0x", 0) == 0 || hex.rfind("0X", 0) == 0) {
        return hex.substr(2);
    }
    return hex;
}

using CreateClientFn = char* (*)(char*, char*, int, int, long long);
using CreateAuthTokenFn = struct StrOrErr (*)(long long, int, long long);
using SignCreateOrderFn = struct SignedTxResponse (*)(int, long long, long long, int, int, int, int, int, int, long long, long long, int, int, long long, int, long long);
using FreeFn = void (*)(void*);

struct StrOrErr {
    char* str;
    char* err;
};

struct SignedTxResponse {
    std::uint8_t txType;
    char* txInfo;
    char* txHash;
    char* messageToSign;
    char* err;
};

class LighterSignerHandle {
  public:
    explicit LighterSignerHandle(const std::string& dylib_path) {
        handle_ = dlopen(dylib_path.c_str(), RTLD_NOW);
        if (handle_ == nullptr) {
            throw std::runtime_error(dlerror());
        }
        create_client_ = load<CreateClientFn>("CreateClient");
        sign_create_order_ = load<SignCreateOrderFn>("SignCreateOrder");
        free_ = load<FreeFn>("Free");
    }

    ~LighterSignerHandle() {
        if (handle_ != nullptr) {
            dlclose(handle_);
        }
    }

    template <typename Fn>
    Fn load(const char* name) {
        void* symbol = dlsym(handle_, name);
        if (symbol == nullptr) {
            throw std::runtime_error(std::string("missing symbol: ") + name);
        }
        return reinterpret_cast<Fn>(symbol);
    }

    CreateClientFn create_client() const { return create_client_; }
    SignCreateOrderFn sign_create_order() const { return sign_create_order_; }
    FreeFn free_fn() const { return free_; }

  private:
    void* handle_ {nullptr};
    CreateClientFn create_client_ {nullptr};
    SignCreateOrderFn sign_create_order_ {nullptr};
    FreeFn free_ {nullptr};
};

std::string decode_and_free(char* ptr, FreeFn free_fn) {
    if (ptr == nullptr) {
        return {};
    }
    std::string value(ptr);
    free_fn(ptr);
    return value;
}

}  // namespace

NativeHyperliquidTrading::NativeHyperliquidTrading(HyperliquidConfig config) : config_(std::move(config)) {}

Bbo NativeHyperliquidTrading::get_bbo(const std::string& coin) {
    const std::string payload = "{\"type\":\"l2Book\",\"coin\":\"" + coin + "\",\"nSigFigs\":5}";
    const HttpResponse response = http_post(config_.api_url + "/info", payload, {{"Content-Type", "application/json"}});
    const std::regex bid_pattern(R"REGEX("levels":\[\[\{"px":"([^"]+)")REGEX");
    const std::regex ask_pattern(R"REGEX(\],\[\{"px":"([^"]+)")REGEX");
    return Bbo {
        .bid = first_match_as_double(response.body, bid_pattern),
        .ask = first_match_as_double(response.body, ask_pattern),
        .quote_age_ms = 0,
    };
}

HlLimitOrderAck NativeHyperliquidTrading::place_limit_order(const HlLimitOrderRequest& request) {
    if (request.dry_run) {
        return HlLimitOrderAck {.ok = true, .message = "dry-run", .oid = "dry_oid"};
    }
    const auto& meta = meta_for_coin(request.coin);
    const std::uint64_t nonce = current_timestamp_ms();
    const std::string action_json = order_action_json(request, meta, false, false);
    const Bytes32 hash = order_action_hash(request, meta, false, false);
    const std::string body = post_exchange_action(action_json, hash, nonce);

    const std::regex resting(R"REGEX("resting":\{"oid":([0-9]+)\})REGEX");
    const std::regex filled(R"REGEX("filled":\{[^}]*"oid":([0-9]+))REGEX");
    std::smatch match;
    if (std::regex_search(body, match, resting) || std::regex_search(body, match, filled)) {
        return HlLimitOrderAck {.ok = true, .message = "ok", .oid = match[1].str()};
    }
    return HlLimitOrderAck {.ok = false, .message = body, .oid = ""};
}

HlCancelAck NativeHyperliquidTrading::cancel_order(const std::string& coin, const std::string& oid, bool dry_run) {
    if (dry_run) {
        return HlCancelAck {.ok = true, .message = "dry-run", .oid = oid};
    }
    const std::uint64_t nonce = current_timestamp_ms();
    const std::string action_json = cancel_action_json(coin, oid);
    const Bytes32 hash = cancel_action_hash(coin, oid);
    const std::string body = post_exchange_action(action_json, hash, nonce);
    return HlCancelAck {.ok = body.find("\"status\":\"ok\"") != std::string::npos, .message = body, .oid = oid};
}

HlReduceAck NativeHyperliquidTrading::reduce_position(const std::string& coin, bool is_buy, double size, bool dry_run) {
    if (dry_run) {
        return HlReduceAck {.ok = true, .message = "dry-run", .filled_size = size, .avg_fill_price = get_bbo(coin).mid()};
    }
    const Bbo bbo = get_bbo(coin);
    HlLimitOrderRequest request {
        .coin = coin,
        .is_buy = is_buy,
        .price = is_buy ? bbo.ask * 1.003 : bbo.bid * 0.997,
        .size = size,
        .post_only = false,
        .dry_run = false,
    };
    const auto& meta = meta_for_coin(coin);
    const std::uint64_t nonce = current_timestamp_ms();
    const std::string action_json = order_action_json(request, meta, true, true);
    const Bytes32 hash = order_action_hash(request, meta, true, true);
    const std::string body = post_exchange_action(action_json, hash, nonce);
    const std::regex filled_size_pattern(R"REGEX("totalSz":"([^"]+)")REGEX");
    const std::regex avg_px_pattern(R"REGEX("avgPx":"([^"]+)")REGEX");
    return HlReduceAck {
        .ok = body.find("\"status\":\"ok\"") != std::string::npos,
        .message = body,
        .filled_size = body.find("totalSz") != std::string::npos ? first_match_as_double(body, filled_size_pattern) : 0.0,
        .avg_fill_price = body.find("avgPx") != std::string::npos ? first_match_as_double(body, avg_px_pattern) : 0.0,
    };
}

const NativeHyperliquidTrading::MetaEntry& NativeHyperliquidTrading::meta_for_coin(const std::string& coin) const {
    ensure_meta();
    const auto it = meta_.find(coin);
    if (it == meta_.end()) {
        throw std::runtime_error("missing HL meta for coin " + coin);
    }
    return it->second;
}

std::string NativeHyperliquidTrading::post_exchange_action(const std::string& action_json, const Bytes32& action_hash, std::uint64_t nonce) const {
    const std::string signature = sign_l1_action(action_hash);
    const std::ostringstream payload;
    const std::string nonce_str = std::to_string(nonce);
    std::string body = "{\"action\":" + action_json +
        ",\"nonce\":" + nonce_str +
        ",\"signature\":" + signature +
        ",\"vaultAddress\":null,\"expiresAfter\":null}";
    const HttpResponse response = http_post(config_.api_url + "/exchange", body, {{"Content-Type", "application/json"}});
    return response.body;
}

std::string NativeHyperliquidTrading::sign_l1_action(const Bytes32& action_hash) const {
    const std::vector<std::uint8_t> priv = hex_to_bytes(config_.private_key);
    if (priv.size() != 32) {
        throw std::runtime_error("HL private key must be 32 bytes");
    }

    const Bytes32 domain_typehash = keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)");
    const Bytes32 name_hash = keccak256("Exchange");
    const Bytes32 version_hash = keccak256("1");
    std::vector<std::uint8_t> domain_data;
    append_word(domain_data, domain_typehash);
    append_word(domain_data, name_hash);
    append_word(domain_data, version_hash);
    append_word(domain_data, uint256_word(1337));
    append_word(domain_data, address_word());
    const Bytes32 domain_separator = keccak256(domain_data);

    const Bytes32 agent_typehash = keccak256("Agent(string source,bytes32 connectionId)");
    const Bytes32 source_hash = keccak256("a");
    std::vector<std::uint8_t> msg_data;
    append_word(msg_data, agent_typehash);
    append_word(msg_data, source_hash);
    append_word(msg_data, action_hash);
    const Bytes32 message_hash = keccak256(msg_data);

    std::vector<std::uint8_t> digest_bytes;
    digest_bytes.push_back(0x19);
    digest_bytes.push_back(0x01);
    digest_bytes.insert(digest_bytes.end(), domain_separator.begin(), domain_separator.end());
    digest_bytes.insert(digest_bytes.end(), message_hash.begin(), message_hash.end());
    const Bytes32 digest = keccak256(digest_bytes);

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    secp256k1_ecdsa_recoverable_signature sig;
    if (secp256k1_ecdsa_sign_recoverable(ctx, &sig, digest.data(), priv.data(), nullptr, nullptr) != 1) {
        secp256k1_context_destroy(ctx);
        throw std::runtime_error("HL signature failed");
    }
    unsigned char compact[64];
    int recid = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(ctx, compact, &recid, &sig);
    secp256k1_context_destroy(ctx);

    return "{\"r\":\"" + bytes_to_hex(compact, 32) +
        "\",\"s\":\"" + bytes_to_hex(compact + 32, 32) +
        "\",\"v\":" + std::to_string(27 + recid) + "}";
}

std::string NativeHyperliquidTrading::order_action_json(const HlLimitOrderRequest& request, const MetaEntry& meta, bool ioc, bool reduce_only) const {
    const std::string tif = ioc ? "Ioc" : (request.post_only ? "Alo" : "Gtc");
    return "{\"type\":\"order\",\"orders\":[{\"a\":" + std::to_string(meta.asset) +
        ",\"b\":" + std::string(request.is_buy ? "true" : "false") +
        ",\"p\":\"" + float_to_wire(round_sig_figs(request.price, 5)) +
        "\",\"s\":\"" + float_to_wire(std::round(request.size * std::pow(10.0, meta.sz_decimals)) / std::pow(10.0, meta.sz_decimals)) +
        "\",\"r\":" + std::string(reduce_only ? "true" : "false") +
        ",\"t\":{\"limit\":{\"tif\":\"" + tif + "\"}}}],\"grouping\":\"na\"}";
}

std::string NativeHyperliquidTrading::cancel_action_json(const std::string& coin, const std::string& oid) const {
    const auto& meta = meta_for_coin(coin);
    return "{\"type\":\"cancel\",\"cancels\":[{\"a\":" + std::to_string(meta.asset) + ",\"o\":" + oid + "}]}";
}

Bytes32 NativeHyperliquidTrading::order_action_hash(const HlLimitOrderRequest& request, const MetaEntry& meta, bool ioc, bool reduce_only) const {
    ByteBuffer out;
    mp_pack_map_size(out, 3);
    mp_pack_str(out, "type");
    mp_pack_str(out, "order");
    mp_pack_str(out, "orders");
    mp_pack_array_size(out, 1);
    mp_pack_map_size(out, 5);
    mp_pack_str(out, "a");
    mp_pack_uint(out, static_cast<std::uint64_t>(meta.asset));
    mp_pack_str(out, "b");
    mp_pack_bool(out, request.is_buy);
    mp_pack_str(out, "p");
    mp_pack_str(out, float_to_wire(round_sig_figs(request.price, 5)));
    mp_pack_str(out, "s");
    mp_pack_str(out, float_to_wire(std::round(request.size * std::pow(10.0, meta.sz_decimals)) / std::pow(10.0, meta.sz_decimals)));
    mp_pack_str(out, "r");
    mp_pack_bool(out, reduce_only);
    mp_pack_str(out, "t");
    mp_pack_map_size(out, 1);
    mp_pack_str(out, "limit");
    mp_pack_map_size(out, 1);
    mp_pack_str(out, "tif");
    mp_pack_str(out, ioc ? "Ioc" : (request.post_only ? "Alo" : "Gtc"));
    mp_pack_str(out, "grouping");
    mp_pack_str(out, "na");

    const std::uint64_t nonce = current_timestamp_ms();
    out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&nonce), reinterpret_cast<const std::uint8_t*>(&nonce) + 8);
    std::reverse(out.end() - 8, out.end());
    out.push_back(0x00);
    return keccak256(out);
}

Bytes32 NativeHyperliquidTrading::cancel_action_hash(const std::string& coin, const std::string& oid) const {
    const auto& meta = meta_for_coin(coin);
    ByteBuffer out;
    mp_pack_map_size(out, 2);
    mp_pack_str(out, "type");
    mp_pack_str(out, "cancel");
    mp_pack_str(out, "cancels");
    mp_pack_array_size(out, 1);
    mp_pack_map_size(out, 2);
    mp_pack_str(out, "a");
    mp_pack_uint(out, static_cast<std::uint64_t>(meta.asset));
    mp_pack_str(out, "o");
    mp_pack_uint(out, static_cast<std::uint64_t>(std::stoull(oid)));
    const std::uint64_t nonce = current_timestamp_ms();
    out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&nonce), reinterpret_cast<const std::uint8_t*>(&nonce) + 8);
    std::reverse(out.end() - 8, out.end());
    out.push_back(0x00);
    return keccak256(out);
}

std::string NativeHyperliquidTrading::float_to_wire(double value) {
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(8);
    oss << value;
    std::string out = oss.str();
    while (!out.empty() && out.back() == '0') {
        out.pop_back();
    }
    if (!out.empty() && out.back() == '.') {
        out.pop_back();
    }
    if (out.empty()) {
        return "0";
    }
    return out;
}

double NativeHyperliquidTrading::round_sig_figs(double value, int sig_figs) {
    if (value == 0.0) {
        return 0.0;
    }
    const double scale = std::pow(10.0, sig_figs - 1 - std::floor(std::log10(std::fabs(value))));
    return std::round(value * scale) / scale;
}

void NativeHyperliquidTrading::ensure_meta() const {
    if (!meta_.empty()) {
        return;
    }
    const HttpResponse response = http_post(config_.api_url + "/info", "{\"type\":\"meta\"}", {{"Content-Type", "application/json"}});
    std::regex entry_pattern(R"REGEX(\{"szDecimals":([0-9]+),"name":"([^"]+)")REGEX");
    auto begin = std::sregex_iterator(response.body.begin(), response.body.end(), entry_pattern);
    auto end = std::sregex_iterator();
    int asset = 0;
    for (auto it = begin; it != end; ++it, ++asset) {
        meta_.emplace((*it)[2].str(), MetaEntry {.asset = asset, .sz_decimals = std::stoi((*it)[1].str())});
    }
}

NativeLighterTrading::NativeLighterTrading(LighterConfig config) : config_(std::move(config)) {}

NativeLighterTrading::~NativeLighterTrading() {
    if (signer_lib_ != nullptr) {
        dlclose(signer_lib_);
    }
}

Bbo NativeLighterTrading::get_bbo(std::int64_t market_id) {
    const HttpResponse response = http_get(config_.api_url + "/api/v1/orderBookOrders?market_id=" + std::to_string(market_id) + "&limit=5");
    const std::regex ask_pattern(R"REGEX("asks":\[\{[^}]*"price":"([^"]+)")REGEX");
    const std::regex bid_pattern(R"REGEX("bids":\[\{[^}]*"price":"([^"]+)")REGEX");
    return Bbo {
        .bid = first_match_as_double(response.body, bid_pattern),
        .ask = first_match_as_double(response.body, ask_pattern),
        .quote_age_ms = 0,
    };
}

LighterIocAck NativeLighterTrading::place_ioc_order(const LighterIocRequest& request) {
    if (request.dry_run) {
        return LighterIocAck {.ok = true, .message = "dry-run", .tx_hash = "dry_tx"};
    }
    ensure_client();

    const auto next_nonce_value = next_nonce();
    const auto* signer = static_cast<LighterSignerHandle*>(signer_lib_);
    const auto result = signer->sign_create_order()(
        static_cast<int>(config_.market_index),
        static_cast<long long>(current_timestamp_ms()),
        static_cast<long long>(scaled_size(request.size)),
        static_cast<int>(scaled_price(request.price)),
        request.is_ask ? 1 : 0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        static_cast<long long>(next_nonce_value),
        config_.api_key_index,
        config_.account_index
    );

    const std::string err = decode_and_free(result.err, signer->free_fn());
    const std::string tx_info = decode_and_free(result.txInfo, signer->free_fn());
    const std::string tx_hash = decode_and_free(result.txHash, signer->free_fn());
    decode_and_free(result.messageToSign, signer->free_fn());
    if (!err.empty()) {
        return LighterIocAck {.ok = false, .message = err, .tx_hash = ""};
    }

    const std::string body = "tx_type=" + std::to_string(result.txType) + "&tx_info=" + json_escape(tx_info);
    const HttpResponse response = http_post(
        config_.api_url + "/api/v1/sendTx",
        body,
        {{"Content-Type", "application/x-www-form-urlencoded"}}
    );

    if (response.body.find("\"code\":200") != std::string::npos || response.body.find("\"tx_hash\"") != std::string::npos) {
        const std::regex tx_hash_pattern(R"REGEX("tx_hash":"([^"]+)")REGEX");
        return LighterIocAck {
            .ok = true,
            .message = response.body,
            .tx_hash = response.body.find("tx_hash") != std::string::npos ? first_match_as_string(response.body, tx_hash_pattern) : tx_hash,
        };
    }
    return LighterIocAck {.ok = false, .message = response.body, .tx_hash = tx_hash};
}

void NativeLighterTrading::ensure_client() {
    if (client_ready_) {
        return;
    }
    if (signer_lib_ == nullptr) {
        signer_lib_ = new LighterSignerHandle("third_party/lighter_signer/lighter-signer-darwin-arm64.dylib");
    }
    auto* signer = static_cast<LighterSignerHandle*>(signer_lib_);
    std::string url = config_.api_url;
    std::string key = trim_0x(config_.api_private_key);
    char* err_ptr = signer->create_client()(url.data(), key.data(), 304, config_.api_key_index, config_.account_index);
    const std::string err = decode_and_free(err_ptr, signer->free_fn());
    if (!err.empty()) {
        throw std::runtime_error(err);
    }

    const HttpResponse response = http_get(config_.api_url + "/api/v1/orderBookDetails?market_id=" + std::to_string(config_.market_index));
    const std::regex price_dec_pattern(R"REGEX("price_decimals":([0-9]+))REGEX");
    const std::regex size_dec_pattern(R"REGEX("size_decimals":([0-9]+))REGEX");
    const std::regex min_base_pattern(R"REGEX("min_base_amount":"([^"]+)")REGEX");
    price_decimals_ = static_cast<int>(first_match_as_double(response.body, price_dec_pattern));
    size_decimals_ = static_cast<int>(first_match_as_double(response.body, size_dec_pattern));
    min_base_amount_ = first_match_as_double(response.body, min_base_pattern);
    client_ready_ = true;
}

std::uint64_t NativeLighterTrading::next_nonce() const {
    const HttpResponse response = http_get(
        config_.api_url + "/api/v1/nextNonce?account_index=" + std::to_string(config_.account_index) +
        "&api_key_index=" + std::to_string(config_.api_key_index)
    );
    const std::regex nonce_pattern(R"REGEX("nonce":([0-9]+))REGEX");
    return static_cast<std::uint64_t>(first_match_as_double(response.body, nonce_pattern));
}

std::int64_t NativeLighterTrading::scaled_size(double size) const {
    const double clamped = std::max(size, min_base_amount_);
    return static_cast<std::int64_t>(std::llround(clamped * std::pow(10.0, size_decimals_)));
}

std::uint32_t NativeLighterTrading::scaled_price(double price) const {
    return static_cast<std::uint32_t>(std::llround(price * std::pow(10.0, price_decimals_)));
}

std::string NativeLighterTrading::json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

}  // namespace arb

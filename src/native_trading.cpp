#include "arb/native_trading.hpp"

#include "arb/http.hpp"
#include "arb/msgpack.hpp"
#include "arb/perf.hpp"

#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include <dlfcn.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>

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

std::string url_encode(const std::string& value) {
    std::ostringstream out;
    out << std::hex << std::uppercase;
    for (const unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.'
            || ch == '~') {
            out << static_cast<char>(ch);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }
    return out.str();
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
        create_auth_token_ = load<CreateAuthTokenFn>("CreateAuthToken");
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
    CreateAuthTokenFn create_auth_token() const { return create_auth_token_; }
    SignCreateOrderFn sign_create_order() const { return sign_create_order_; }
    FreeFn free_fn() const { return free_; }

  private:
    void* handle_ {nullptr};
    CreateClientFn create_client_ {nullptr};
    CreateAuthTokenFn create_auth_token_ {nullptr};
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

void NativeHyperliquidTrading::set_action_transport(ActionTransport transport) {
    action_transport_ = std::move(transport);
}

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
    const Bytes32 hash = order_action_hash(request, meta, false, false, nonce);
    const HlActionResult result = post_exchange_action(action_json, hash, nonce);
    const std::string& body = result.body;

    const std::regex resting(R"REGEX("resting":\{"oid":([0-9]+)\})REGEX");
    const std::regex filled(R"REGEX("filled":\{[^}]*"oid":([0-9]+))REGEX");
    std::smatch match;
    if (std::regex_search(body, match, resting) || std::regex_search(body, match, filled)) {
        return HlLimitOrderAck {
            .ok = true,
            .message = "ok",
            .oid = match[1].str(),
            .sign_latency_ms = result.sign_latency_ms,
            .ws_send_call_latency_ms = result.ws_send_call_latency_ms,
            .ws_send_to_response_rx_latency_ms = result.ws_send_to_response_rx_latency_ms,
            .response_rx_to_unblock_latency_ms = result.response_rx_to_unblock_latency_ms,
        };
    }
    return HlLimitOrderAck {
        .ok = false,
        .message = body,
        .oid = "",
        .sign_latency_ms = result.sign_latency_ms,
        .ws_send_call_latency_ms = result.ws_send_call_latency_ms,
        .ws_send_to_response_rx_latency_ms = result.ws_send_to_response_rx_latency_ms,
        .response_rx_to_unblock_latency_ms = result.response_rx_to_unblock_latency_ms,
    };
}

HlCancelAck NativeHyperliquidTrading::cancel_order(const std::string& coin, const std::string& oid, bool dry_run) {
    if (dry_run) {
        return HlCancelAck {.ok = true, .message = "dry-run", .oid = oid};
    }
    const std::uint64_t nonce = current_timestamp_ms();
    const std::string action_json = cancel_action_json(coin, oid);
    const Bytes32 hash = cancel_action_hash(coin, oid, nonce);
    const std::string body = post_exchange_action(action_json, hash, nonce).body;
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
    const Bytes32 hash = order_action_hash(request, meta, true, true, nonce);
    const std::string body = post_exchange_action(action_json, hash, nonce).body;
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

NativeHyperliquidTrading::HlActionResult NativeHyperliquidTrading::post_exchange_action(const std::string& action_json, const Bytes32& action_hash, std::uint64_t nonce) const {
    const std::uint64_t sign_start_ns = perf_now_ns();
    const std::string signature = sign_l1_action(action_hash);
    const std::uint64_t sign_end_ns = perf_now_ns();
    const std::ostringstream payload;
    const std::string nonce_str = std::to_string(nonce);
    const std::string vault_part = config_.vault_address.has_value()
        ? "\"" + config_.vault_address.value() + "\""
        : "null";
    std::string body = "{\"action\":" + action_json +
        ",\"nonce\":" + nonce_str +
        ",\"signature\":" + signature +
        ",\"vaultAddress\":" + vault_part + ",\"expiresAfter\":null}";
    if (!action_transport_) {
        throw std::runtime_error("Hyperliquid action transport unavailable: WS post transport is required");
    }
    HlActionTransportResult transport = action_transport_(body);
    return HlActionResult {
        .body = std::move(transport.body),
        .sign_latency_ms = static_cast<double>(sign_end_ns - sign_start_ns) / 1000000.0,
        .ws_send_call_latency_ms = transport.send_call_latency_ms,
        .ws_send_to_response_rx_latency_ms = transport.send_to_response_rx_latency_ms,
        .response_rx_to_unblock_latency_ms = transport.response_rx_to_unblock_latency_ms,
    };
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

void NativeHyperliquidTrading::append_vault_and_nonce(ByteBuffer& buf, std::uint64_t nonce) const {
    // Append nonce as big-endian 8 bytes
    for (int i = 7; i >= 0; --i) {
        buf.push_back(static_cast<std::uint8_t>((nonce >> (8 * i)) & 0xff));
    }
    // vault_address flag + optional 20-byte address
    if (config_.vault_address.has_value()) {
        buf.push_back(0x01);
        const auto addr_bytes = hex_to_bytes(config_.vault_address.value());
        buf.insert(buf.end(), addr_bytes.begin(), addr_bytes.end());
    } else {
        buf.push_back(0x00);
    }
}

Bytes32 NativeHyperliquidTrading::order_action_hash(const HlLimitOrderRequest& request, const MetaEntry& meta, bool ioc, bool reduce_only, std::uint64_t nonce) const {
    ByteBuffer out;
    mp_pack_map_size(out, 3);
    mp_pack_str(out, "type");
    mp_pack_str(out, "order");
    mp_pack_str(out, "orders");
    mp_pack_array_size(out, 1);
    mp_pack_map_size(out, 6);
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

    append_vault_and_nonce(out, nonce);
    return keccak256(out);
}

Bytes32 NativeHyperliquidTrading::cancel_action_hash(const std::string& coin, const std::string& oid, std::uint64_t nonce) const {
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

    append_vault_and_nonce(out, nonce);
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
        delete static_cast<LighterSignerHandle*>(signer_lib_);
    }
}

void NativeLighterTrading::set_tx_transport(TxTransport transport) {
    tx_transport_ = std::move(transport);
}

void NativeLighterTrading::set_position_waiter(PositionWaiter waiter) {
    position_waiter_ = std::move(waiter);
}

std::string NativeLighterTrading::create_auth_token(std::int64_t deadline_ms) {
    ensure_client();
    const auto* signer = static_cast<LighterSignerHandle*>(signer_lib_);
    if (signer == nullptr) {
        throw std::runtime_error("lighter signer not initialized");
    }
    const auto token = signer->create_auth_token()(deadline_ms, config_.api_key_index, config_.account_index);
    const std::string err = decode_and_free(token.err, signer->free_fn());
    if (!err.empty()) {
        throw std::runtime_error(err);
    }
    return decode_and_free(token.str, signer->free_fn());
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
        return LighterIocAck {
            .ok = true,
            .fill_confirmed = true,
            .message = "dry-run",
            .tx_hash = "dry_tx",
            .confirmed_size = request.size,
            .fill_price = request.price,
            .fee = 0.0,
            .nonce_fetch_latency_ms = 0.0,
            .sign_order_latency_ms = 0.0,
            .send_tx_ack_latency_ms = 0.0,
            .place_to_http_ack_latency_ms = 0.0,
            .http_ack_latency_ms = 0.0,
            .fill_confirm_latency_ms = 0.0,
            .confirm_attempts = 0,
        };
    }
    ensure_client();

    // Snapshot position BEFORE sending order (with avg_entry for fill price calc)
    const auto snap_before = query_position_snapshot();
    const double pos_before = snap_before.size;
    const std::uint64_t submit_start_ns = perf_now_ns();

    const auto next_nonce_value = next_nonce();
    const std::uint64_t nonce_done_ns = perf_now_ns();
    const std::uint64_t nonce_fetch_latency_ns = nonce_done_ns - submit_start_ns;
    PerfCollector::instance().record_trade_path(
        PerfMetric::LighterNonceFetchNs,
        nonce_fetch_latency_ns
    );
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
    const std::uint64_t sign_done_ns = perf_now_ns();
    const std::uint64_t sign_order_latency_ns = sign_done_ns - nonce_done_ns;
    PerfCollector::instance().record_trade_path(
        PerfMetric::LighterSignOrderNs,
        sign_order_latency_ns
    );
    if (!err.empty()) {
        return LighterIocAck {
            .ok = false,
            .fill_confirmed = false,
            .message = err,
            .tx_hash = "",
            .confirmed_size = 0.0,
            .nonce_fetch_latency_ms = static_cast<double>(nonce_fetch_latency_ns) / 1000000.0,
            .sign_order_latency_ms = static_cast<double>(sign_order_latency_ns) / 1000000.0,
            .send_tx_ack_latency_ms = 0.0,
            .place_to_http_ack_latency_ms = 0.0,
            .http_ack_latency_ms = 0.0,
            .fill_confirm_latency_ms = 0.0,
            .confirm_attempts = 0,
        };
    }

    std::cerr << "[lighter-debug] tx_type=" << static_cast<int>(result.txType)
              << " tx_info_raw=" << tx_info.substr(0, 200) << '\n';

    const std::uint64_t send_start_ns = perf_now_ns();
    std::string response_body;
    if (tx_transport_) {
        response_body = tx_transport_(result.txType, tx_info);
    } else {
        const std::string body = "tx_type=" + std::to_string(result.txType) + "&tx_info=" + url_encode(tx_info);
        const HttpResponse response = http_post(
            config_.api_url + "/api/v1/sendTx",
            body,
            {{"Content-Type", "application/x-www-form-urlencoded"}}
        );
        response_body = response.body;
    }
    const std::uint64_t http_ack_ns = perf_now_ns();
    const std::uint64_t send_tx_ack_latency_ns = http_ack_ns - send_start_ns;
    const std::uint64_t http_ack_latency_ns = http_ack_ns - submit_start_ns;
    PerfCollector::instance().record_trade_path(
        PerfMetric::LighterSendTxAckNs,
        send_tx_ack_latency_ns
    );
    PerfCollector::instance().record_trade_path(
        PerfMetric::LighterSendToHttpAckNs,
        http_ack_latency_ns
    );
    std::cerr << "[lighter-debug] response=" << response_body << '\n';

    const bool tx_accepted = response_body.find("\"code\":200") != std::string::npos
                          || response_body.find("\"tx_hash\"") != std::string::npos;
    if (!tx_accepted) {
        return LighterIocAck {
            .ok = false,
            .fill_confirmed = false,
            .message = response_body,
            .tx_hash = tx_hash,
            .confirmed_size = 0.0,
            .nonce_fetch_latency_ms = static_cast<double>(nonce_fetch_latency_ns) / 1000000.0,
            .sign_order_latency_ms = static_cast<double>(sign_order_latency_ns) / 1000000.0,
            .send_tx_ack_latency_ms = static_cast<double>(send_tx_ack_latency_ns) / 1000000.0,
            .place_to_http_ack_latency_ms = static_cast<double>(http_ack_latency_ns) / 1000000.0,
            .http_ack_latency_ms = static_cast<double>(http_ack_latency_ns) / 1000000.0,
            .fill_confirm_latency_ms = 0.0,
            .confirm_attempts = 0,
        };
    }

    const std::regex tx_hash_pattern(R"REGEX("tx_hash":"([^"]+)")REGEX");
    const std::string confirmed_tx_hash = response_body.find("tx_hash") != std::string::npos
        ? first_match_as_string(response_body, tx_hash_pattern)
        : tx_hash;

    // Wait for a position update event from the account WS feed, then verify position changed.
    const std::regex exec_time_pattern(R"REGEX("predicted_execution_time_ms":([0-9]+))REGEX");
    std::uint64_t wait_ms = 500;  // Default wait
    std::smatch exec_match;
    if (std::regex_search(response_body, exec_match, exec_time_pattern)) {
        const std::uint64_t predicted = std::stoull(exec_match[1].str());
        const std::uint64_t now = current_timestamp_ms();
        if (predicted > now) {
            wait_ms = predicted - now + 200;  // predicted + 200ms buffer
        }
    }

    bool fill_confirmed = false;
    double confirmed_size = 0.0;
    double fill_price = 0.0;
    int confirm_attempts = 0;
    std::uint64_t fill_confirm_latency_ns = 0;
    std::optional<LighterPositionSnapshot> snap_after_opt;
    if (position_waiter_) {
        confirm_attempts = 1;
        snap_after_opt = position_waiter_(pos_before, static_cast<int>(std::min(wait_ms + 1000, static_cast<std::uint64_t>(3000))));
    }
    if (!snap_after_opt.has_value()) {
        confirm_attempts += 1;
        const auto snap_after = query_position_snapshot();
        if (std::abs(snap_after.size - pos_before) > 0.001) {
            snap_after_opt = snap_after;
        }
    }

    if (snap_after_opt.has_value()) {
        const auto& snap_after = *snap_after_opt;
        const double delta = std::abs(snap_after.size - pos_before);
        fill_confirmed = delta > 0.001;
        confirmed_size = delta;
        // Detect if position is increasing or decreasing
        const bool pos_increasing = std::abs(snap_after.size) > std::abs(pos_before) + 0.001;
        if (pos_increasing) {
            // Position increased: avg_entry changes → value-based calc is accurate
            const double value_before = std::abs(pos_before) * snap_before.avg_entry_price;
            const double value_after = std::abs(snap_after.size) * snap_after.avg_entry_price;
            if (delta > 0.0001) {
                fill_price = std::abs(value_after - value_before) / delta;
            }
        } else {
            // Position decreased: avg_entry stays same → value-based calc returns avg_entry (WRONG)
            // Use signal_price (Lighter mid at hedge time) as best approximation of real fill price
            fill_price = request.signal_price > 0.0 ? request.signal_price : snap_before.avg_entry_price;
        }
        fill_confirm_latency_ns = perf_now_ns() - http_ack_ns;
        PerfCollector::instance().record_trade_path(
            PerfMetric::LighterHttpAckToFillConfirmNs,
            fill_confirm_latency_ns
        );
        std::cerr << "[lighter-debug] fill CONFIRMED: pos " << pos_before << " -> " << snap_after.size
                  << " delta=" << delta << " fill_px=" << fill_price
                  << " avg_entry " << snap_before.avg_entry_price << " -> " << snap_after.avg_entry_price
                  << " attempts=" << confirm_attempts << '\n';
    } else {
        fill_confirm_latency_ns = perf_now_ns() - http_ack_ns;
        PerfCollector::instance().record_trade_path(
            PerfMetric::LighterHttpAckToFillConfirmNs,
            fill_confirm_latency_ns
        );
        std::cerr << "[lighter-debug] ⚠️ fill UNCONFIRMED after event wait, tx=" << confirmed_tx_hash << "\n";
    }

    return LighterIocAck {
        .ok = tx_accepted,
        .fill_confirmed = fill_confirmed,
        .message = response_body,
        .tx_hash = confirmed_tx_hash,
        .confirmed_size = confirmed_size,
        .fill_price = fill_price,
        .fee = 0.0,  // Lighter currently charges 0 fees
        .nonce_fetch_latency_ms = static_cast<double>(nonce_fetch_latency_ns) / 1000000.0,
        .sign_order_latency_ms = static_cast<double>(sign_order_latency_ns) / 1000000.0,
        .send_tx_ack_latency_ms = static_cast<double>(send_tx_ack_latency_ns) / 1000000.0,
        .place_to_http_ack_latency_ms = static_cast<double>(http_ack_latency_ns) / 1000000.0,
        .http_ack_latency_ms = static_cast<double>(http_ack_latency_ns) / 1000000.0,
        .fill_confirm_latency_ms = static_cast<double>(fill_confirm_latency_ns) / 1000000.0,
        .confirm_attempts = confirm_attempts,
    };
}

LighterPositionSnapshot NativeLighterTrading::query_position_snapshot() const {
    const HttpResponse response = http_get(
        config_.api_url + "/api/v1/account?by=index&value=" + std::to_string(config_.account_index)
    );
    const std::string& body = response.body;

    const std::string market_key = "\"market_id\":" + std::to_string(config_.market_index);
    const auto market_pos = body.find(market_key);
    if (market_pos == std::string::npos) {
        return {};  // No position in this market
    }

    const std::string section = body.substr(market_pos, 600);
    const std::regex sign_pattern(R"REGEX("sign":(-?[0-9]+))REGEX");
    const std::regex pos_pattern(R"REGEX("position":"([^"]+)")REGEX");
    const std::regex avg_pattern(R"REGEX("avg_entry_price":"([^"]+)")REGEX");
    const std::regex val_pattern(R"REGEX("position_value":"([^"]+)")REGEX");

    std::smatch match;
    LighterPositionSnapshot snap;
    int sign = 0;
    double position = 0.0;
    if (std::regex_search(section, match, sign_pattern)) {
        sign = std::stoi(match[1].str());
    }
    if (std::regex_search(section, match, pos_pattern)) {
        position = std::stod(match[1].str());
    }
    snap.size = sign * position;
    if (std::regex_search(section, match, avg_pattern)) {
        snap.avg_entry_price = std::stod(match[1].str());
    }
    if (std::regex_search(section, match, val_pattern)) {
        snap.position_value = std::stod(match[1].str());
    }
    return snap;
}

double NativeLighterTrading::query_position() const {
    return query_position_snapshot().size;
}

void NativeLighterTrading::ensure_client() {
    if (client_ready_) {
        return;
    }
    if (signer_lib_ == nullptr) {
#if defined(__APPLE__)
        signer_lib_ = new LighterSignerHandle("third_party/lighter_signer/lighter-signer-darwin-arm64.dylib");
#elif defined(__linux__)
        signer_lib_ = new LighterSignerHandle("third_party/lighter_signer/lighter-signer-linux-arm64.so");
#else
#error "Unsupported platform for lighter-signer"
#endif
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
    refresh_nonce_cache();
    client_ready_ = true;
}

std::uint64_t NativeLighterTrading::fetch_remote_next_nonce() const {
    const HttpResponse response = http_get(
        config_.api_url + "/api/v1/nextNonce?account_index=" + std::to_string(config_.account_index) +
        "&api_key_index=" + std::to_string(config_.api_key_index)
    );
    const std::regex nonce_pattern(R"REGEX("nonce":([0-9]+))REGEX");
    return static_cast<std::uint64_t>(first_match_as_double(response.body, nonce_pattern));
}

void NativeLighterTrading::refresh_nonce_cache() const {
    const std::uint64_t remote_nonce = fetch_remote_next_nonce();
    nonce_cache_.store(remote_nonce, std::memory_order_release);
}

std::uint64_t NativeLighterTrading::next_nonce() const {
    const std::uint64_t cached = nonce_cache_.load(std::memory_order_acquire);
    if (cached != 0) {
        return nonce_cache_.fetch_add(1, std::memory_order_acq_rel);
    }

    const std::uint64_t remote_nonce = fetch_remote_next_nonce();
    std::uint64_t expected = 0;
    if (nonce_cache_.compare_exchange_strong(expected, remote_nonce + 1, std::memory_order_acq_rel)) {
        return remote_nonce;
    }
    return nonce_cache_.fetch_add(1, std::memory_order_acq_rel);
}

std::int64_t NativeLighterTrading::scaled_size(double size) const {
    const double clamped = std::max(size, min_base_amount_);
    return static_cast<std::int64_t>(std::llround(clamped * std::pow(10.0, size_decimals_)));
}

std::uint32_t NativeLighterTrading::scaled_price(double price) const {
    return static_cast<std::uint32_t>(std::llround(price * std::pow(10.0, price_decimals_)));
}

std::string NativeLighterTrading::json_escape(const std::string& value) {
    // URL-encode for application/x-www-form-urlencoded
    std::ostringstream encoded;
    encoded.fill('0');
    encoded << std::hex;
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded << ch;
        } else {
            encoded << '%' << std::setw(2) << std::uppercase << static_cast<int>(ch);
        }
    }
    return encoded.str();
}

}  // namespace arb

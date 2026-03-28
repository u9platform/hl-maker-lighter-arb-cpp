#pragma once

#include "arb/crypto.hpp"
#include "arb/exchange.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace arb {

struct HyperliquidConfig {
    std::string api_url {"https://api.hyperliquid.xyz"};
    std::string private_key;
    std::optional<std::string> vault_address;
    std::string coin {"HYPE"};
};

struct LighterConfig {
    std::string api_url {"https://mainnet.zklighter.elliot.ai"};
    std::string api_private_key;
    std::int64_t account_index {0};
    int api_key_index {0};
    std::int64_t market_index {24};
};

class NativeHyperliquidTrading final : public HyperliquidExchange {
  public:
    using ActionTransport = std::function<HlActionTransportResult(const std::string&)>;

    explicit NativeHyperliquidTrading(HyperliquidConfig config);

    void set_action_transport(ActionTransport transport);

    [[nodiscard]] Bbo get_bbo(const std::string& coin) override;
    [[nodiscard]] HlLimitOrderAck place_limit_order(const HlLimitOrderRequest& request) override;
    [[nodiscard]] HlCancelAck cancel_order(const std::string& coin, const std::string& oid, bool dry_run) override;
    [[nodiscard]] HlReduceAck reduce_position(const std::string& coin, bool is_buy, double size, bool dry_run) override;

  private:
    struct MetaEntry {
        int asset {0};
        int sz_decimals {0};
    };

    [[nodiscard]] const MetaEntry& meta_for_coin(const std::string& coin) const;
    struct HlActionResult {
        std::string body;
        double sign_latency_ms {0.0};
        double ws_send_call_latency_ms {0.0};
        double ws_send_to_response_rx_latency_ms {0.0};
        double response_rx_to_unblock_latency_ms {0.0};
    };

    [[nodiscard]] HlActionResult post_exchange_action(const std::string& action_json, const Bytes32& action_hash, std::uint64_t nonce) const;
    [[nodiscard]] std::string sign_l1_action(const Bytes32& action_hash) const;
    [[nodiscard]] std::string order_action_json(const HlLimitOrderRequest& request, const MetaEntry& meta, bool ioc, bool reduce_only) const;
    [[nodiscard]] std::string cancel_action_json(const std::string& coin, const std::string& oid) const;
    [[nodiscard]] Bytes32 order_action_hash(const HlLimitOrderRequest& request, const MetaEntry& meta, bool ioc, bool reduce_only, std::uint64_t nonce) const;
    [[nodiscard]] Bytes32 cancel_action_hash(const std::string& coin, const std::string& oid, std::uint64_t nonce) const;
    void append_vault_and_nonce(std::vector<std::uint8_t>& buf, std::uint64_t nonce) const;
    [[nodiscard]] static std::string float_to_wire(double value);
    [[nodiscard]] static double round_sig_figs(double value, int sig_figs);
    void ensure_meta() const;

    HyperliquidConfig config_;
    ActionTransport action_transport_;
    mutable std::unordered_map<std::string, MetaEntry> meta_;
};

class NativeLighterTrading final : public LighterExchange {
  public:
    using TxTransport = std::function<std::string(std::uint8_t tx_type, const std::string& tx_info_json)>;
    using PositionWaiter = std::function<std::optional<LighterPositionSnapshot>(double baseline_size, int timeout_ms)>;

    explicit NativeLighterTrading(LighterConfig config);
    ~NativeLighterTrading() override;

    void set_tx_transport(TxTransport transport);
    void set_position_waiter(PositionWaiter waiter);
    [[nodiscard]] std::string create_auth_token(std::int64_t deadline_ms);

    [[nodiscard]] Bbo get_bbo(std::int64_t market_id) override;
    [[nodiscard]] LighterIocAck place_ioc_order(const LighterIocRequest& request) override;

  private:
    void ensure_client();
    [[nodiscard]] std::uint64_t fetch_remote_next_nonce() const;
    void refresh_nonce_cache() const;
    [[nodiscard]] std::uint64_t next_nonce() const;
    [[nodiscard]] std::int64_t scaled_size(double size) const;
    [[nodiscard]] std::uint32_t scaled_price(double price) const;
    [[nodiscard]] static std::string json_escape(const std::string& value);
    [[nodiscard]] LighterPositionSnapshot query_position_snapshot() const;
    [[nodiscard]] double query_position() const;

    LighterConfig config_;
    TxTransport tx_transport_;
    PositionWaiter position_waiter_;
    void* signer_lib_ {nullptr};
    bool client_ready_ {false};
    int price_decimals_ {4};
    int size_decimals_ {2};
    double min_base_amount_ {0.0};
    mutable std::atomic<std::uint64_t> nonce_cache_ {0};
};

}  // namespace arb

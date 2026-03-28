#pragma once

#include "arb/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace arb {

struct HlActionTransportResult {
    std::string body;
    double send_call_latency_ms {0.0};
    double send_to_response_rx_latency_ms {0.0};
    double response_rx_to_unblock_latency_ms {0.0};
};

struct HlLimitOrderRequest {
    std::string coin {"HYPE"};
    bool is_buy {true};
    double price {0.0};
    double size {0.0};
    bool post_only {true};
    bool dry_run {true};
};

struct HlLimitOrderAck {
    bool ok {false};
    std::string message;
    std::string oid;
    double sign_latency_ms {0.0};
    double ws_send_call_latency_ms {0.0};
    double ws_send_to_response_rx_latency_ms {0.0};
    double response_rx_to_unblock_latency_ms {0.0};
};

struct HlCancelAck {
    bool ok {false};
    std::string message;
    std::string oid;
};

struct HlReduceAck {
    bool ok {false};
    std::string message;
    double filled_size {0.0};
    double avg_fill_price {0.0};
};

struct LighterIocRequest {
    bool is_ask {false};
    double price {0.0};
    double size {0.0};
    bool dry_run {true};
};

struct LighterIocAck {
    bool ok {false};
    bool fill_confirmed {false};  // True only if position change verified
    std::string message;
    std::string tx_hash;
    double confirmed_size {0.0};  // Actual size filled (from position delta)
    double fill_price {0.0};      // Actual fill price (from value delta / size delta)
    double fee {0.0};             // Lighter fee (currently 0 for maker/taker on Lighter)
    double nonce_fetch_latency_ms {0.0};
    double sign_order_latency_ms {0.0};
    double send_tx_ack_latency_ms {0.0};
    double place_to_http_ack_latency_ms {0.0};
    double http_ack_latency_ms {0.0};
    double fill_confirm_latency_ms {0.0};
    int confirm_attempts {0};
};

struct FillEvent {
    std::string venue;
    std::string order_id;
    double fill_price {0.0};
    double fill_size {0.0};
};

class HyperliquidExchange {
  public:
    virtual ~HyperliquidExchange() = default;

    [[nodiscard]] virtual Bbo get_bbo(const std::string& coin) = 0;
    [[nodiscard]] virtual HlLimitOrderAck place_limit_order(const HlLimitOrderRequest& request) = 0;
    [[nodiscard]] virtual HlCancelAck cancel_order(const std::string& coin, const std::string& oid, bool dry_run) = 0;
    [[nodiscard]] virtual HlReduceAck reduce_position(const std::string& coin, bool is_buy, double size, bool dry_run) = 0;
  };

class LighterExchange {
  public:
    virtual ~LighterExchange() = default;

    [[nodiscard]] virtual Bbo get_bbo(std::int64_t market_id) = 0;
    [[nodiscard]] virtual LighterIocAck place_ioc_order(const LighterIocRequest& request) = 0;
  };

}  // namespace arb

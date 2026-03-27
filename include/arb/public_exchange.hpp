#pragma once

#include "arb/exchange.hpp"

#include <string>

namespace arb {

class NativeHyperliquidExchange final : public HyperliquidExchange {
  public:
    explicit NativeHyperliquidExchange(std::string api_url = "https://api.hyperliquid.xyz");

    [[nodiscard]] Bbo get_bbo(const std::string& coin) override;
    [[nodiscard]] HlLimitOrderAck place_limit_order(const HlLimitOrderRequest& request) override;
    [[nodiscard]] HlCancelAck cancel_order(const std::string& coin, const std::string& oid, bool dry_run) override;
    [[nodiscard]] HlReduceAck reduce_position(const std::string& coin, bool is_buy, double size, bool dry_run) override;

  private:
    std::string api_url_;
};

class NativeLighterExchange final : public LighterExchange {
  public:
    explicit NativeLighterExchange(std::string api_url = "https://mainnet.zklighter.elliot.ai");

    [[nodiscard]] Bbo get_bbo(std::int64_t market_id) override;
    [[nodiscard]] LighterIocAck place_ioc_order(const LighterIocRequest& request) override;

  private:
    std::string api_url_;
};

}  // namespace arb

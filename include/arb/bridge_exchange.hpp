#pragma once

#include "arb/exchange.hpp"

#include <string>

namespace arb {

class BridgeHyperliquidExchange final : public HyperliquidExchange {
  public:
    explicit BridgeHyperliquidExchange(std::string bridge_script, std::string python_bin = "python3");

    [[nodiscard]] Bbo get_bbo(const std::string& coin) override;
    [[nodiscard]] HlLimitOrderAck place_limit_order(const HlLimitOrderRequest& request) override;
    [[nodiscard]] HlCancelAck cancel_order(const std::string& coin, const std::string& oid, bool dry_run) override;
    [[nodiscard]] HlReduceAck reduce_position(const std::string& coin, bool is_buy, double size, bool dry_run) override;

  private:
    [[nodiscard]] std::string run_bridge_command(const std::string& args) const;

    std::string bridge_script_;
    std::string python_bin_;
};

class BridgeLighterExchange final : public LighterExchange {
  public:
    explicit BridgeLighterExchange(std::string bridge_script, std::string python_bin = "python3");

    [[nodiscard]] Bbo get_bbo(std::int64_t market_id) override;
    [[nodiscard]] LighterIocAck place_ioc_order(const LighterIocRequest& request) override;

  private:
    [[nodiscard]] std::string run_bridge_command(const std::string& args) const;

    std::string bridge_script_;
    std::string python_bin_;
};

}  // namespace arb

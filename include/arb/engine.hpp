#pragma once

#include "arb/exchange.hpp"
#include "arb/strategy.hpp"

#include <optional>
#include <string>
#include <vector>

namespace arb {

struct EngineConfig {
    StrategyConfig strategy;
    std::string hl_coin {"HYPE"};
    std::int64_t lighter_market_id {24};
    bool dry_run {true};
};

struct EventLog {
    std::string message;
};

class MakerHedgeEngine {
  public:
    MakerHedgeEngine(
        EngineConfig config,
        HyperliquidExchange& hl,
        LighterExchange& lighter
    );

    [[nodiscard]] SpreadSnapshot collect_snapshot() const;
    [[nodiscard]] std::vector<EventLog> on_market_data(std::int64_t now_ms);
    [[nodiscard]] std::vector<EventLog> on_hl_fill(double fill_price, double fill_size_base, const SpreadSnapshot& snapshot);
    [[nodiscard]] std::vector<EventLog> on_lighter_hedge_reject();
    void on_lighter_hedge_fill(double fill_price);

    [[nodiscard]] const std::optional<std::string>& active_hl_oid() const noexcept;
    [[nodiscard]] const HlMakerLighterHedger& strategy() const noexcept;

  private:
    [[nodiscard]] std::vector<EventLog> execute_action(const Action& action, const SpreadSnapshot& snapshot);

    EngineConfig config_;
    HyperliquidExchange& hl_;
    LighterExchange& lighter_;
    HlMakerLighterHedger strategy_;
    std::optional<std::string> active_hl_oid_;
};

}  // namespace arb

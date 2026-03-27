#pragma once

#include "arb/types.hpp"

#include <optional>

namespace arb {

class HlMakerLighterHedger {
  public:
    explicit HlMakerLighterHedger(StrategyConfig config);

    [[nodiscard]] StrategyState state() const noexcept;
    [[nodiscard]] const StrategyConfig& config() const noexcept;
    [[nodiscard]] const std::optional<PendingMakerOrder>& pending_maker() const noexcept;
    [[nodiscard]] const std::optional<OpenHedgePosition>& open_position() const noexcept;

    Action on_market_snapshot(const SpreadSnapshot& snapshot, std::int64_t now_ms);
    Action on_hl_maker_fill(double fill_price, double fill_size_base, const SpreadSnapshot& snapshot);
    void on_lighter_hedge_fill(double fill_price);
    Action on_lighter_hedge_reject();
    void reset();

  private:
    [[nodiscard]] bool can_arm(std::int64_t now_ms) const noexcept;
    [[nodiscard]] double cancel_threshold_bps() const noexcept;
    [[nodiscard]] Direction direction_for_spread(double cross_spread_bps) const noexcept;
    [[nodiscard]] PendingMakerOrder build_maker_order(const SpreadSnapshot& snapshot) const;
    [[nodiscard]] HedgeIntent build_lighter_hedge(const SpreadSnapshot& snapshot) const;

    StrategyConfig config_;
    StrategyState state_ {StrategyState::Idle};
    std::optional<PendingMakerOrder> pending_maker_;
    std::optional<OpenHedgePosition> open_position_;
    std::int64_t last_disarm_ms_ {0};
};

}  // namespace arb

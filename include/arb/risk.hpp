#pragma once

#include "arb/types.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace arb {

struct RiskConfig {
    double max_exposure_usd {200.0};
    std::int64_t stale_quote_kill_ms {5000};     // Kill switch if no update > N ms.
    int max_consecutive_hedge_failures {3};
    double max_single_trade_usd {50.0};
};

enum class RiskVeto {
    Ok,
    MaxExposure,
    StaleQuotes,
    HedgeFailuresExceeded,
    TradeTooLarge,
    KillSwitchActive,
};

/// Risk guard that sits between strategy intents and execution.
class RiskGuard {
  public:
    explicit RiskGuard(RiskConfig config);

    /// Check if a new maker placement is allowed.
    [[nodiscard]] RiskVeto check_new_entry(double size_usd, const SpreadSnapshot& snap) const;

    /// Report a hedge failure.
    void on_hedge_failure();

    /// Report a successful hedge.
    void on_hedge_success();

    /// Activate kill switch (manual or automatic).
    void activate_kill_switch(const std::string& reason);

    /// Deactivate kill switch.
    void reset_kill_switch();

    [[nodiscard]] bool kill_switch_active() const noexcept;
    [[nodiscard]] const std::string& kill_switch_reason() const noexcept;
    [[nodiscard]] int consecutive_hedge_failures() const noexcept;
    [[nodiscard]] double current_exposure_usd() const noexcept;

    /// Update current exposure from external state.
    void set_exposure(double exposure_usd);

    [[nodiscard]] static const char* veto_name(RiskVeto veto);

  private:
    RiskConfig config_;
    double current_exposure_usd_ {0.0};
    int consecutive_hedge_failures_ {0};
    bool kill_switch_ {false};
    std::string kill_switch_reason_;
};

}  // namespace arb

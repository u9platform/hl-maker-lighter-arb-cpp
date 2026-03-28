#include "arb/risk.hpp"

#include <cmath>

namespace arb {

RiskGuard::RiskGuard(RiskConfig config) : config_(std::move(config)) {}

RiskVeto RiskGuard::check_new_entry(double size_usd, const SpreadSnapshot& snap) const {
    if (kill_switch_) {
        return RiskVeto::KillSwitchActive;
    }

    if (consecutive_hedge_failures_ >= config_.max_consecutive_hedge_failures) {
        return RiskVeto::HedgeFailuresExceeded;
    }

    if (size_usd > config_.max_single_trade_usd) {
        return RiskVeto::TradeTooLarge;
    }

    if (current_exposure_usd_ + size_usd > config_.max_exposure_usd) {
        return RiskVeto::MaxExposure;
    }

    if (snap.lighter.quote_age_ms > config_.stale_quote_kill_ms ||
        snap.hl.quote_age_ms > config_.stale_quote_kill_ms) {
        return RiskVeto::StaleQuotes;
    }

    return RiskVeto::Ok;
}

void RiskGuard::on_hedge_failure() {
    ++consecutive_hedge_failures_;
    if (consecutive_hedge_failures_ >= config_.max_consecutive_hedge_failures) {
        activate_kill_switch("consecutive hedge failures exceeded");
    }
}

void RiskGuard::on_hedge_success() {
    consecutive_hedge_failures_ = 0;
}

void RiskGuard::activate_kill_switch(const std::string& reason) {
    kill_switch_ = true;
    kill_switch_reason_ = reason;
}

void RiskGuard::reset_kill_switch() {
    kill_switch_ = false;
    kill_switch_reason_.clear();
    consecutive_hedge_failures_ = 0;
}

bool RiskGuard::kill_switch_active() const noexcept {
    return kill_switch_;
}

const std::string& RiskGuard::kill_switch_reason() const noexcept {
    return kill_switch_reason_;
}

int RiskGuard::consecutive_hedge_failures() const noexcept {
    return consecutive_hedge_failures_;
}

double RiskGuard::current_exposure_usd() const noexcept {
    return current_exposure_usd_;
}

void RiskGuard::set_exposure(double exposure_usd) {
    current_exposure_usd_ = std::abs(exposure_usd);
}

const char* RiskGuard::veto_name(RiskVeto veto) {
    switch (veto) {
        case RiskVeto::Ok: return "Ok";
        case RiskVeto::MaxExposure: return "MaxExposure";
        case RiskVeto::StaleQuotes: return "StaleQuotes";
        case RiskVeto::HedgeFailuresExceeded: return "HedgeFailuresExceeded";
        case RiskVeto::TradeTooLarge: return "TradeTooLarge";
        case RiskVeto::KillSwitchActive: return "KillSwitchActive";
    }
    return "Unknown";
}

}  // namespace arb

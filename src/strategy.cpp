#include "arb/strategy.hpp"

#include <cmath>
#include <stdexcept>

namespace arb {

namespace {

double aggressive_lighter_price(const SpreadSnapshot& snapshot, Direction direction) {
    constexpr double kTakerSlippageBps = 10.0;
    const double lighter_mid = snapshot.lighter.mid();
    if (lighter_mid <= 0.0) {
        return 0.0;
    }

    if (direction == Direction::ShortLighterLongHl) {
        return lighter_mid * (1.0 - (kTakerSlippageBps / 10000.0));
    }
    return lighter_mid * (1.0 + (kTakerSlippageBps / 10000.0));
}

}  // namespace

HlMakerLighterHedger::HlMakerLighterHedger(StrategyConfig config) : config_(config) {
    if (config_.spread_bps <= 0.0) {
        throw std::invalid_argument("spread_bps must be positive");
    }
    if (config_.cancel_band_bps < 0.0) {
        throw std::invalid_argument("cancel_band_bps must be non-negative");
    }
}

StrategyState HlMakerLighterHedger::state() const noexcept {
    return state_;
}

const StrategyConfig& HlMakerLighterHedger::config() const noexcept {
    return config_;
}

const std::optional<PendingMakerOrder>& HlMakerLighterHedger::pending_maker() const noexcept {
    return pending_maker_;
}

const std::optional<OpenHedgePosition>& HlMakerLighterHedger::open_position() const noexcept {
    return open_position_;
}

Action HlMakerLighterHedger::on_market_snapshot(const SpreadSnapshot& snapshot, std::int64_t now_ms, double hl_position_base) {
    if (!snapshot.valid(config_.max_quote_age_ms)) {
        return {};
    }

    const double abs_spread = std::abs(snapshot.cross_spread_bps);
    const double threshold = effective_spread_bps(snapshot.cross_spread_bps, hl_position_base);

    if (state_ == StrategyState::Idle) {
        if (abs_spread < threshold || !can_arm(now_ms)) {
            return {};
        }

        pending_maker_ = build_maker_order(snapshot);
        pending_maker_->entry_spread_bps = threshold;  // remember which threshold was used
        state_ = StrategyState::PendingHlMaker;

        Action action;
        action.type = ActionType::PlaceHlMaker;
        action.reason = (threshold < config_.spread_bps) ? "spread reached close threshold" : "spread reached entry threshold";
        action.maker_order = pending_maker_;
        return action;
    }

    if (state_ == StrategyState::PendingHlMaker && pending_maker_.has_value()) {
        const double cancel_thresh = cancel_threshold_bps(pending_maker_->entry_spread_bps);
        if (abs_spread < cancel_thresh) {
            Action action;
            action.type = ActionType::CancelHlMaker;
            action.reason = "spread mean-reverted below cancel threshold";
            action.maker_order = pending_maker_;

            // BUG FIX: Do NOT reset pending_maker_ here.
            // A cancel request is sent to HL, but the order may have already been
            // filled before the cancel arrives. If we reset pending_maker_, the
            // subsequent on_hl_maker_fill() will find no pending order and silently
            // drop the fill — creating a naked position with no hedge.
            // pending_maker_ will be reset when:
            //   a) on_hl_maker_fill() processes the fill and sends the hedge, OR
            //   b) the next on_market_snapshot() places a new maker order (overwriting it)
            state_ = StrategyState::CancelledPendingConfirm;
            last_disarm_ms_ = now_ms;
            return action;
        }
    }

    // If we sent a cancel but haven't confirmed it yet, check if we should re-arm.
    if (state_ == StrategyState::CancelledPendingConfirm) {
        if (abs_spread >= threshold && can_arm(now_ms)) {
            // Spread widened again — place a new maker order (overwrite old pending_maker_)
            pending_maker_ = build_maker_order(snapshot);
            pending_maker_->entry_spread_bps = threshold;
            state_ = StrategyState::PendingHlMaker;

            Action action;
            action.type = ActionType::PlaceHlMaker;
            action.reason = "spread re-widened after cancel";
            action.maker_order = pending_maker_;
            return action;
        }
    }

    return {};
}

Action HlMakerLighterHedger::on_hl_maker_fill(
    double fill_price,
    double fill_size_base,
    const SpreadSnapshot& snapshot
) {
    if (!pending_maker_.has_value()) {
        return {};
    }

    open_position_ = OpenHedgePosition {
        .direction = pending_maker_->direction,
        .size_base = fill_size_base,
        .hl_fill_price = fill_price,
        .lighter_fill_price = 0.0,
    };
    state_ = StrategyState::HlFilledPendingLighterHedge;

    Action action;
    action.type = ActionType::SendLighterTakerHedge;
    action.reason = "hl maker filled";
    action.hedge_intent = build_lighter_hedge(snapshot);

    pending_maker_.reset();
    return action;
}

void HlMakerLighterHedger::on_lighter_hedge_fill(double fill_price) {
    if (!open_position_.has_value()) {
        return;
    }

    open_position_->lighter_fill_price = fill_price;
    state_ = StrategyState::Open;
}

Action HlMakerLighterHedger::on_lighter_hedge_reject() {
    if (!open_position_.has_value()) {
        return {};
    }

    state_ = StrategyState::UnwindingHl;

    Action action;
    action.type = ActionType::UnwindHlPosition;
    action.reason = "lighter hedge failed after hl maker fill";
    return action;
}

void HlMakerLighterHedger::reset() {
    pending_maker_.reset();
    open_position_.reset();
    state_ = StrategyState::Idle;
}

bool HlMakerLighterHedger::can_arm(std::int64_t now_ms) const noexcept {
    return (now_ms - last_disarm_ms_) >= config_.min_rearm_ms;
}

double HlMakerLighterHedger::effective_spread_bps(double cross_spread_bps, double hl_position_base) const noexcept {
    const double close_bps = config_.close_spread_bps > 0.0 ? config_.close_spread_bps : config_.spread_bps;
    if (close_bps >= config_.spread_bps) {
        return config_.spread_bps;  // no benefit, use open threshold
    }

    // Determine if this trade direction would reduce position
    const Direction dir = direction_for_spread(cross_spread_bps);
    // ShortLighterLongHl = buy on HL (reduces short, increases long)
    // LongLighterShortHl = sell on HL (reduces long, increases short)
    const bool would_reduce = (hl_position_base < 0.0 && dir == Direction::ShortLighterLongHl) ||
                              (hl_position_base > 0.0 && dir == Direction::LongLighterShortHl);

    return would_reduce ? close_bps : config_.spread_bps;
}

double HlMakerLighterHedger::cancel_threshold_bps(double entry_spread_bps) const noexcept {
    const double threshold = entry_spread_bps - config_.cancel_band_bps;
    return threshold > 0.0 ? threshold : 0.0;
}

Direction HlMakerLighterHedger::direction_for_spread(double cross_spread_bps) const noexcept {
    if (cross_spread_bps >= 0.0) {
        return Direction::ShortLighterLongHl;
    }
    return Direction::LongLighterShortHl;
}

PendingMakerOrder HlMakerLighterHedger::build_maker_order(const SpreadSnapshot& snapshot) const {
    const Direction direction = direction_for_spread(snapshot.cross_spread_bps);
    const double avg_mid = (snapshot.lighter.mid() + snapshot.hl.mid()) / 2.0;
    const double size_base = avg_mid > 0.0 ? config_.pair_size_usd / avg_mid : 0.0;

    if (direction == Direction::ShortLighterLongHl) {
        return PendingMakerOrder {
            .direction = direction,
            .is_buy = true,
            .price = snapshot.hl.bid,
            .size_base = size_base,
            .trigger_spread_bps = snapshot.cross_spread_bps,
        };
    }

    return PendingMakerOrder {
        .direction = direction,
        .is_buy = false,
        .price = snapshot.hl.ask,
        .size_base = size_base,
        .trigger_spread_bps = snapshot.cross_spread_bps,
    };
}

HedgeIntent HlMakerLighterHedger::build_lighter_hedge(const SpreadSnapshot& snapshot) const {
    if (!open_position_.has_value()) {
        return {};
    }

    const bool is_ask = open_position_->direction == Direction::ShortLighterLongHl;
    return HedgeIntent {
        .is_ask = is_ask,
        .limit_price = aggressive_lighter_price(snapshot, open_position_->direction),
        .size_base = open_position_->size_base,
    };
}

}  // namespace arb

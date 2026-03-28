#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace arb {

enum class Direction {
    ShortLighterLongHl,
    LongLighterShortHl,
};

enum class StrategyState {
    Idle,
    PendingHlMaker,
    CancelledPendingConfirm,  // Cancel sent but fill may still arrive; pending_maker_ retained.
    HlFilledPendingLighterHedge,
    Open,
    UnwindingHl,
    Error,
};

struct Bbo {
    double bid {0.0};
    double ask {0.0};
    std::int64_t quote_age_ms {0};

    [[nodiscard]] double mid() const noexcept {
        if (bid <= 0.0 || ask <= 0.0) {
            return 0.0;
        }
        return (bid + ask) / 2.0;
    }
};

struct SpreadSnapshot {
    Bbo lighter;
    Bbo hl;
    double cross_spread_bps {0.0};

    [[nodiscard]] bool valid(std::int64_t max_quote_age_ms) const noexcept {
        return lighter.mid() > 0.0 && hl.mid() > 0.0 &&
               lighter.quote_age_ms <= max_quote_age_ms &&
               hl.quote_age_ms <= max_quote_age_ms;
    }
};

struct StrategyConfig {
    double spread_bps {2.0};
    double cancel_band_bps {0.5};
    double pair_size_usd {25.0};
    double max_position_usd {100.0};
    std::int64_t max_quote_age_ms {1500};
    std::int64_t min_rearm_ms {500};
};

struct PendingMakerOrder {
    Direction direction {Direction::ShortLighterLongHl};
    bool is_buy {true};
    double price {0.0};
    double size_base {0.0};
    double trigger_spread_bps {0.0};
};

struct OpenHedgePosition {
    Direction direction {Direction::ShortLighterLongHl};
    double size_base {0.0};
    double hl_fill_price {0.0};
    double lighter_fill_price {0.0};
};

struct HedgeIntent {
    bool is_ask {false};
    double limit_price {0.0};
    double size_base {0.0};
};

enum class ActionType {
    None,
    PlaceHlMaker,
    CancelHlMaker,
    SendLighterTakerHedge,
    UnwindHlPosition,
};

struct Action {
    ActionType type {ActionType::None};
    std::string reason;
    std::optional<PendingMakerOrder> maker_order;
    std::optional<HedgeIntent> hedge_intent;
};

}  // namespace arb

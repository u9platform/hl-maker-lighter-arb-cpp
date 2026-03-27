#include "arb/strategy.hpp"

#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

std::string_view state_name(arb::StrategyState state) {
    switch (state) {
        case arb::StrategyState::Idle:
            return "Idle";
        case arb::StrategyState::PendingHlMaker:
            return "PendingHlMaker";
        case arb::StrategyState::HlFilledPendingLighterHedge:
            return "HlFilledPendingLighterHedge";
        case arb::StrategyState::Open:
            return "Open";
        case arb::StrategyState::UnwindingHl:
            return "UnwindingHl";
        case arb::StrategyState::Error:
            return "Error";
    }
    return "Unknown";
}

void print_action(const arb::Action& action) {
    std::cout << "action=" << static_cast<int>(action.type) << " reason=" << action.reason << '\n';
    if (action.maker_order.has_value()) {
        std::cout << "  maker price=" << action.maker_order->price
                  << " size=" << action.maker_order->size_base
                  << " is_buy=" << action.maker_order->is_buy << '\n';
    }
    if (action.hedge_intent.has_value()) {
        std::cout << "  hedge price=" << action.hedge_intent->limit_price
                  << " size=" << action.hedge_intent->size_base
                  << " is_ask=" << action.hedge_intent->is_ask << '\n';
    }
}

}  // namespace

int main() {
    arb::StrategyConfig config {
        .spread_bps = 2.0,
        .cancel_band_bps = 0.5,
        .pair_size_usd = 25.0,
        .max_position_usd = 100.0,
        .max_quote_age_ms = 1000,
        .min_rearm_ms = 250,
    };

    arb::HlMakerLighterHedger strategy(config);

    arb::SpreadSnapshot entry_snapshot {
        .lighter = {.bid = 10.03, .ask = 10.05, .quote_age_ms = 5},
        .hl = {.bid = 10.00, .ask = 10.01, .quote_age_ms = 4},
        .cross_spread_bps = 3.49,
    };

    const arb::Action place = strategy.on_market_snapshot(entry_snapshot, std::int64_t {1'000});
    std::cout << "state=" << state_name(strategy.state()) << '\n';
    print_action(place);

    const arb::Action hedge = strategy.on_hl_maker_fill(10.00, 2.5, entry_snapshot);
    std::cout << "state=" << state_name(strategy.state()) << '\n';
    print_action(hedge);

    strategy.on_lighter_hedge_fill(10.03);
    std::cout << "state=" << state_name(strategy.state()) << '\n';

    arb::SpreadSnapshot cancel_snapshot {
        .lighter = {.bid = 10.01, .ask = 10.02, .quote_age_ms = 5},
        .hl = {.bid = 10.00, .ask = 10.01, .quote_age_ms = 5},
        .cross_spread_bps = 0.99,
    };
    const arb::Action cancel = strategy.on_market_snapshot(cancel_snapshot, std::int64_t {1'500});
    print_action(cancel);

    return 0;
}

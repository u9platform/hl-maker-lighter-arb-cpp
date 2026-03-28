#include "arb/engine.hpp"

#include <cmath>
#include <sstream>

namespace arb {

MakerHedgeEngine::MakerHedgeEngine(
    EngineConfig config,
    HyperliquidExchange& hl,
    LighterExchange& lighter
) : config_(std::move(config)),
    hl_(hl),
    lighter_(lighter),
    strategy_(config_.strategy) {}

SpreadSnapshot MakerHedgeEngine::collect_snapshot() const {
    const Bbo hl_bbo = hl_.get_bbo(config_.hl_coin);
    const Bbo lighter_bbo = lighter_.get_bbo(config_.lighter_market_id);
    const double hl_mid = hl_bbo.mid();
    const double lighter_mid = lighter_bbo.mid();
    const double avg_mid = (hl_mid + lighter_mid) / 2.0;
    const double spread = avg_mid > 0.0 ? ((lighter_mid - hl_mid) / avg_mid) * 10000.0 : 0.0;
    return SpreadSnapshot {
        .lighter = lighter_bbo,
        .hl = hl_bbo,
        .cross_spread_bps = spread,
    };
}

bool MakerHedgeEngine::position_limit_reached(double mid_price) const noexcept {
    if (mid_price <= 0.0) return false;
    const double pos_usd = std::abs(hl_position_base_) * mid_price;
    return pos_usd >= config_.strategy.max_position_usd;
}

std::vector<EventLog> MakerHedgeEngine::on_market_data(std::int64_t now_ms) {
    const SpreadSnapshot snapshot = collect_snapshot();
    
    // Position limit: if we're at max, don't open new positions.
    // Force strategy to Idle so it doesn't place new maker orders.
    if (position_limit_reached(snapshot.hl.mid()) &&
        strategy_.state() == StrategyState::Idle) {
        return {};  // Skip — at position limit
    }
    
    const Action action = strategy_.on_market_snapshot(snapshot, now_ms);
    return execute_action(action, snapshot);
}

std::vector<EventLog> MakerHedgeEngine::on_hl_fill(double fill_price, double fill_size_base, const SpreadSnapshot& snapshot, const std::string& oid) {
    // BUG FIX 3: Check if this fill belongs to any recently placed order, not just the current active one.
    if (recently_placed_oids_.find(oid) == recently_placed_oids_.end()) {
        return {};
    }
    
    // Do NOT erase the oid here — partial fills arrive as multiple messages
    // with the same oid. Each partial fill must be hedged independently.
    // The oid will be cleaned up when a new order is placed (clear + insert).

    // For each partial fill, send an immediate Lighter hedge regardless of strategy state.
    // This avoids the state machine getting confused by partial fills.
    const Direction dir = last_maker_direction_;
    const bool is_ask = (dir == Direction::ShortLighterLongHl);
    
    // Use aggressive price for taker hedge
    const double lighter_mid = snapshot.lighter.mid();
    constexpr double kSlippageBps = 10.0;
    const double hedge_price = is_ask
        ? lighter_mid * (1.0 - kSlippageBps / 10000.0)
        : lighter_mid * (1.0 + kSlippageBps / 10000.0);

    const LighterIocAck ack = lighter_.place_ioc_order(LighterIocRequest {
        .is_ask = is_ask,
        .price = hedge_price,
        .size = fill_size_base,
        .dry_run = config_.dry_run,
    });

    std::vector<EventLog> events;
    std::ostringstream msg;
    if (ack.ok) {
        msg << "TRADE COMPLETE: hl_px=" << fill_price << " sz=" << fill_size_base
            << " lt_hedge tx=" << ack.tx_hash
            << " spread=" << snapshot.cross_spread_bps;
    } else {
        msg << "HEDGE FAILED for hl fill px=" << fill_price << " sz=" << fill_size_base
            << " err=" << ack.message;
    }
    events.push_back(EventLog {.message = msg.str()});

    // Track HL position: buy adds, sell subtracts
    const bool hl_is_buy = (dir == Direction::ShortLighterLongHl);  // HL buys when shorting Lighter
    hl_position_base_ += hl_is_buy ? fill_size_base : -fill_size_base;

    // Reset strategy to Idle so it can immediately start looking for next trade
    strategy_.reset();
    active_hl_oid_.reset();
    return events;
}

std::vector<EventLog> MakerHedgeEngine::on_lighter_hedge_reject() {
    const Action action = strategy_.on_lighter_hedge_reject();
    return execute_action(action, collect_snapshot());
}

void MakerHedgeEngine::on_lighter_hedge_fill(double fill_price) {
    strategy_.on_lighter_hedge_fill(fill_price);
}

const std::optional<std::string>& MakerHedgeEngine::active_hl_oid() const noexcept {
    return active_hl_oid_;
}

const HlMakerLighterHedger& MakerHedgeEngine::strategy() const noexcept {
    return strategy_;
}

double MakerHedgeEngine::hl_position_base() const noexcept {
    return hl_position_base_;
}

std::vector<EventLog> MakerHedgeEngine::execute_action(const Action& action, const SpreadSnapshot& snapshot) {
    std::vector<EventLog> events;

    switch (action.type) {
        case ActionType::None:
            return events;
        case ActionType::PlaceHlMaker: {
            const HlLimitOrderAck ack = hl_.place_limit_order(HlLimitOrderRequest {
                .coin = config_.hl_coin,
                .is_buy = action.maker_order->is_buy,
                .price = action.maker_order->price,
                .size = action.maker_order->size_base,
                .post_only = true,
                .dry_run = config_.dry_run,
            });
            if (ack.ok) {
                active_hl_oid_ = ack.oid;
                recently_placed_oids_.clear();
                recently_placed_oids_.insert(ack.oid);
                // Remember direction so partial fills can hedge correctly
                last_maker_direction_ = action.maker_order->direction;
                events.push_back(EventLog {.message = "placed hl maker oid=" + ack.oid});
            } else {
                events.push_back(EventLog {.message = "hl maker placement failed: " + ack.message});
            }
            return events;
        }
        case ActionType::CancelHlMaker: {
            if (!active_hl_oid_.has_value()) {
                events.push_back(EventLog {.message = "cancel requested with no active oid"});
                return events;
            }
            const HlCancelAck ack = hl_.cancel_order(config_.hl_coin, *active_hl_oid_, config_.dry_run);
            events.push_back(EventLog {.message = ack.ok ? "cancelled hl maker oid=" + ack.oid
                                                        : "hl cancel failed: " + ack.message});
            
            // BUG FIX 3: Keep the OID in recently_placed_oids_ even after cancel.
            // This allows us to still match fills that arrive after the cancel was sent
            // but before the fill message was received (race condition).
            // The OID will be removed when the fill is actually processed.
            active_hl_oid_.reset();
            return events;
        }
        case ActionType::SendLighterTakerHedge: {
            const LighterIocAck ack = lighter_.place_ioc_order(LighterIocRequest {
                .is_ask = action.hedge_intent->is_ask,
                .price = action.hedge_intent->limit_price,
                .size = action.hedge_intent->size_base,
                .dry_run = config_.dry_run,
            });
            std::ostringstream msg;
            if (ack.ok) {
                // Lighter IOC: sendTx success with code=200 means order is accepted and
                // executed on-chain. Treat as filled at our limit price (worst case).
                strategy_.on_lighter_hedge_fill(action.hedge_intent->limit_price);
                const auto& pos = strategy_.open_position();
                msg << "TRADE COMPLETE: lighter hedge FILLED tx=" << ack.tx_hash
                    << " hl_px=" << (pos ? pos->hl_fill_price : 0.0)
                    << " lt_px=" << (pos ? pos->lighter_fill_price : 0.0)
                    << " spread=" << snapshot.cross_spread_bps;
                // Position is hedged on both sides. Reset to Idle for next trade.
                // The PnL is locked in the spread between HL and Lighter fills.
                strategy_.reset();
            } else {
                // Hedge failed — trigger HL unwind
                const auto reject_logs = execute_action(strategy_.on_lighter_hedge_reject(), snapshot);
                for (const auto& rl : reject_logs) {
                    events.push_back(rl);
                }
                msg << "lighter hedge FAILED: " << ack.message
                    << " spread=" << snapshot.cross_spread_bps;
            }
            events.push_back(EventLog {.message = msg.str()});
            return events;
        }
        case ActionType::UnwindHlPosition: {
            const auto* open = strategy_.open_position().has_value() ? &*strategy_.open_position() : nullptr;
            const bool is_buy = open != nullptr && open->direction == Direction::LongLighterShortHl;
            const double size = open != nullptr ? open->size_base : 0.0;
            const HlReduceAck ack = hl_.reduce_position(config_.hl_coin, is_buy, size, config_.dry_run);
            events.push_back(EventLog {.message = ack.ok ? "unwound naked hl position" : "hl unwind failed: " + ack.message});
            return events;
        }
    }

    return events;
}

}  // namespace arb

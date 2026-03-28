#include "arb/engine.hpp"

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

std::vector<EventLog> MakerHedgeEngine::on_market_data(std::int64_t now_ms) {
    const SpreadSnapshot snapshot = collect_snapshot();
    const Action action = strategy_.on_market_snapshot(snapshot, now_ms);
    return execute_action(action, snapshot);
}

std::vector<EventLog> MakerHedgeEngine::on_hl_fill(double fill_price, double fill_size_base, const SpreadSnapshot& snapshot, const std::string& oid) {
    // BUG FIX 3: Check if this fill belongs to any recently placed order, not just the current active one.
    // This handles the race condition where a cancel is sent and active_hl_oid_ is reset,
    // but the fill message arrives after that reset.
    if (recently_placed_oids_.find(oid) == recently_placed_oids_.end()) {
        // This fill doesn't belong to any of our recent orders, ignore it
        return {};
    }
    
    // Remove from tracking set since we've processed this fill
    recently_placed_oids_.erase(oid);
    
    const Action action = strategy_.on_hl_maker_fill(fill_price, fill_size_base, snapshot);
    return execute_action(action, snapshot);
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
                // BUG FIX 3: Track this OID in the recently placed set to handle fill race conditions
                recently_placed_oids_.insert(ack.oid);
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
            msg << (ack.ok ? "sent lighter hedge tx=" : "lighter hedge failed: ")
                << (ack.ok ? ack.tx_hash : ack.message)
                << " spread=" << snapshot.cross_spread_bps;
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

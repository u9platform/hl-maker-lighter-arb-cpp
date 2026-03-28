#include "arb/engine.hpp"
#include "arb/perf.hpp"

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
    PerfCollector::instance().record_hot_path(
        PerfMetric::CrossVenueAlignmentMs,
        static_cast<std::uint64_t>(std::llabs(snapshot.hl.quote_age_ms - snapshot.lighter.quote_age_ms))
    );
    
    // Position limit: if we're at max, don't open new positions.
    // Force strategy to Idle so it doesn't place new maker orders.
    if (position_limit_reached(snapshot.hl.mid()) &&
        strategy_.state() == StrategyState::Idle) {
        return {};  // Skip — at position limit
    }
    
    const std::uint64_t decision_start_ns = perf_now_ns();
    const Action action = strategy_.on_market_snapshot(snapshot, now_ms);
    const std::uint64_t decision_end_ns = perf_now_ns();
    PerfCollector::instance().record_hot_path(
        PerfMetric::StrategyDecisionNs,
        decision_end_ns - decision_start_ns
    );

    if (action.type == ActionType::PlaceHlMaker) {
        perf_trace_ = {};
        perf_trace_.signal_ns = decision_end_ns;
    } else if (action.type == ActionType::CancelHlMaker) {
        perf_trace_.cancel_trigger_ns = decision_end_ns;
    }
    return execute_action(action, snapshot);
}

std::vector<EventLog> MakerHedgeEngine::on_hl_fill(double fill_price, double fill_size_base, const SpreadSnapshot& snapshot, const std::string& oid, std::uint64_t fill_rx_ns) {
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
    perf_trace_.hl_fill_rx_ns = fill_rx_ns;
    if (perf_trace_.hl_ack_ns > 0 && fill_rx_ns >= perf_trace_.hl_ack_ns) {
        PerfCollector::instance().record_trade_path(
            PerfMetric::HlMakerRestingLifetimeNs,
            fill_rx_ns - perf_trace_.hl_ack_ns
        );
    }
    
    // Use aggressive price for taker hedge
    const double lighter_mid = snapshot.lighter.mid();
    constexpr double kSlippageBps = 10.0;
    const double hedge_price = is_ask
        ? lighter_mid * (1.0 - kSlippageBps / 10000.0)
        : lighter_mid * (1.0 + kSlippageBps / 10000.0);

    const std::uint64_t lighter_send_ns = perf_now_ns();
    perf_trace_.lighter_send_ns = lighter_send_ns;
    PerfCollector::instance().record_trade_path(
        PerfMetric::HlFillLocalRxToLighterSendNs,
        lighter_send_ns - fill_rx_ns
    );

    const LighterIocAck ack = lighter_.place_ioc_order(LighterIocRequest {
        .is_ask = is_ask,
        .price = hedge_price,
        .size = fill_size_base,
        .dry_run = config_.dry_run,
    });
    const std::uint64_t lighter_ack_ns = perf_now_ns();
    perf_trace_.lighter_ack_ns = lighter_ack_ns;
    PerfCollector::instance().record_trade_path(
        PerfMetric::LighterSendToAckNs,
        lighter_ack_ns - lighter_send_ns
    );
    PerfCollector::instance().record_trade_path(
        PerfMetric::MakerFillToTakerAckTotalNs,
        lighter_ack_ns - fill_rx_ns
    );

    std::vector<EventLog> events;
    std::ostringstream msg;
    std::ostringstream perf_msg;
    if (ack.ok && ack.fill_confirmed) {
        msg << "TRADE COMPLETE: hl_px=" << fill_price << " sz=" << fill_size_base
            << " lt_confirmed_sz=" << ack.confirmed_size
            << " lt_hedge tx=" << ack.tx_hash
            << " spread=" << snapshot.cross_spread_bps;
        // Track HL position: buy adds, sell subtracts
        const bool hl_is_buy = (dir == Direction::ShortLighterLongHl);
        hl_position_base_ += hl_is_buy ? fill_size_base : -fill_size_base;
        perf_msg << "perf trade oid=" << oid
                 << " signal_to_hl_send_ms=" << ((perf_trace_.hl_send_ns > perf_trace_.signal_ns)
                        ? static_cast<double>(perf_trace_.hl_send_ns - perf_trace_.signal_ns) / 1000000.0 : 0.0)
                 << " hl_send_to_ack_ms=" << ((perf_trace_.hl_ack_ns > perf_trace_.hl_send_ns)
                        ? static_cast<double>(perf_trace_.hl_ack_ns - perf_trace_.hl_send_ns) / 1000000.0 : 0.0)
                 << " hl_resting_ms=" << ((fill_rx_ns > perf_trace_.hl_ack_ns)
                        ? static_cast<double>(fill_rx_ns - perf_trace_.hl_ack_ns) / 1000000.0 : 0.0)
                 << " hl_fill_rx_to_lt_send_ms=" << static_cast<double>(lighter_send_ns - fill_rx_ns) / 1000000.0
                 << " lt_send_to_ack_ms=" << static_cast<double>(lighter_ack_ns - lighter_send_ns) / 1000000.0
                 << " hedge_total_ms=" << static_cast<double>(lighter_ack_ns - fill_rx_ns) / 1000000.0;
    } else if (ack.ok && !ack.fill_confirmed) {
        msg << "HEDGE UNCONFIRMED: hl_px=" << fill_price << " sz=" << fill_size_base
            << " lt_tx=" << ack.tx_hash << " — UNWINDING HL POSITION";
        events.push_back(EventLog {.message = msg.str()});
        // Lighter fill not confirmed — unwind the HL side to avoid naked position
        const bool unwind_buy = !is_ask;  // Reverse the HL fill direction
        const std::uint64_t unwind_send_ns = perf_now_ns();
        PerfCollector::instance().record_trade_path(
            PerfMetric::HedgeFailureToUnwindSendNs,
            unwind_send_ns - lighter_ack_ns
        );
        const HlReduceAck unwind = hl_.reduce_position(config_.hl_coin, unwind_buy, fill_size_base, config_.dry_run);
        const std::uint64_t unwind_ack_ns = perf_now_ns();
        PerfCollector::instance().record_trade_path(
            PerfMetric::UnwindSendToAckNs,
            unwind_ack_ns - unwind_send_ns
        );
        std::ostringstream unwind_msg;
        unwind_msg << (unwind.ok ? "HL UNWIND OK" : "HL UNWIND FAILED")
                   << " sz=" << unwind.filled_size << " avg_px=" << unwind.avg_fill_price;
        events.push_back(EventLog {.message = unwind_msg.str()});
        std::ostringstream unwind_perf;
        unwind_perf << "perf trade oid=" << oid
                    << " hedge_status=unconfirmed"
                    << " hl_fill_rx_to_lt_send_ms=" << static_cast<double>(lighter_send_ns - fill_rx_ns) / 1000000.0
                    << " lt_send_to_ack_ms=" << static_cast<double>(lighter_ack_ns - lighter_send_ns) / 1000000.0
                    << " hedge_total_ms=" << static_cast<double>(lighter_ack_ns - fill_rx_ns) / 1000000.0
                    << " hedge_fail_to_unwind_send_ms=" << static_cast<double>(unwind_send_ns - lighter_ack_ns) / 1000000.0
                    << " unwind_send_to_ack_ms=" << static_cast<double>(unwind_ack_ns - unwind_send_ns) / 1000000.0;
        events.push_back(EventLog {.message = unwind_perf.str()});
        strategy_.reset();
        active_hl_oid_.reset();
        perf_trace_ = {};
        return events;
    } else {
        msg << "HEDGE FAILED for hl fill px=" << fill_price << " sz=" << fill_size_base
            << " err=" << ack.message << " — UNWINDING HL POSITION";
        events.push_back(EventLog {.message = msg.str()});
        // Hedge failed — unwind HL
        const bool unwind_buy = !is_ask;
        const std::uint64_t unwind_send_ns = perf_now_ns();
        PerfCollector::instance().record_trade_path(
            PerfMetric::HedgeFailureToUnwindSendNs,
            unwind_send_ns - lighter_ack_ns
        );
        const HlReduceAck unwind = hl_.reduce_position(config_.hl_coin, unwind_buy, fill_size_base, config_.dry_run);
        const std::uint64_t unwind_ack_ns = perf_now_ns();
        PerfCollector::instance().record_trade_path(
            PerfMetric::UnwindSendToAckNs,
            unwind_ack_ns - unwind_send_ns
        );
        std::ostringstream unwind_msg;
        unwind_msg << (unwind.ok ? "HL UNWIND OK" : "HL UNWIND FAILED")
                   << " sz=" << unwind.filled_size << " avg_px=" << unwind.avg_fill_price;
        events.push_back(EventLog {.message = unwind_msg.str()});
        std::ostringstream unwind_perf;
        unwind_perf << "perf trade oid=" << oid
                    << " hedge_status=failed"
                    << " hl_fill_rx_to_lt_send_ms=" << static_cast<double>(lighter_send_ns - fill_rx_ns) / 1000000.0
                    << " lt_send_to_ack_ms=" << static_cast<double>(lighter_ack_ns - lighter_send_ns) / 1000000.0
                    << " hedge_total_ms=" << static_cast<double>(lighter_ack_ns - fill_rx_ns) / 1000000.0
                    << " hedge_fail_to_unwind_send_ms=" << static_cast<double>(unwind_send_ns - lighter_ack_ns) / 1000000.0
                    << " unwind_send_to_ack_ms=" << static_cast<double>(unwind_ack_ns - unwind_send_ns) / 1000000.0;
        events.push_back(EventLog {.message = unwind_perf.str()});
        strategy_.reset();
        active_hl_oid_.reset();
        perf_trace_ = {};
        return events;
    }
    events.push_back(EventLog {.message = msg.str()});
    events.push_back(EventLog {.message = perf_msg.str()});

    // Reset strategy to Idle so it can immediately start looking for next trade
    strategy_.reset();
    active_hl_oid_.reset();
    perf_trace_ = {};
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
            const std::uint64_t hl_send_ns = perf_now_ns();
            perf_trace_.hl_send_ns = hl_send_ns;
            if (perf_trace_.signal_ns > 0 && hl_send_ns >= perf_trace_.signal_ns) {
                PerfCollector::instance().record_trade_path(
                    PerfMetric::SignalToHlMakerSendNs,
                    hl_send_ns - perf_trace_.signal_ns
                );
            }
            const HlLimitOrderAck ack = hl_.place_limit_order(HlLimitOrderRequest {
                .coin = config_.hl_coin,
                .is_buy = action.maker_order->is_buy,
                .price = action.maker_order->price,
                .size = action.maker_order->size_base,
                .post_only = true,
                .dry_run = config_.dry_run,
            });
            const std::uint64_t hl_ack_ns = perf_now_ns();
            perf_trace_.hl_ack_ns = hl_ack_ns;
            PerfCollector::instance().record_trade_path(
                PerfMetric::HlMakerSendToAckNs,
                hl_ack_ns - hl_send_ns
            );
            if (ack.ok) {
                active_hl_oid_ = ack.oid;
                recently_placed_oids_.clear();
                recently_placed_oids_.insert(ack.oid);
                perf_trace_.oid = ack.oid;
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
            const std::uint64_t cancel_send_ns = perf_now_ns();
            perf_trace_.cancel_send_ns = cancel_send_ns;
            if (perf_trace_.cancel_trigger_ns > 0 && cancel_send_ns >= perf_trace_.cancel_trigger_ns) {
                PerfCollector::instance().record_trade_path(
                    PerfMetric::CancelTriggerToHlCancelSendNs,
                    cancel_send_ns - perf_trace_.cancel_trigger_ns
                );
            }
            const HlCancelAck ack = hl_.cancel_order(config_.hl_coin, *active_hl_oid_, config_.dry_run);
            const std::uint64_t cancel_ack_ns = perf_now_ns();
            PerfCollector::instance().record_trade_path(
                PerfMetric::HlCancelSendToAckNs,
                cancel_ack_ns - cancel_send_ns
            );
            events.push_back(EventLog {.message = ack.ok ? "cancelled hl maker oid=" + ack.oid
                                                        : "hl cancel failed: " + ack.message});
            std::ostringstream perf_msg;
            perf_msg << "perf cancel oid=" << *active_hl_oid_
                     << " cancel_trigger_to_send_ms=" << ((cancel_send_ns > perf_trace_.cancel_trigger_ns)
                            ? static_cast<double>(cancel_send_ns - perf_trace_.cancel_trigger_ns) / 1000000.0 : 0.0)
                     << " cancel_send_to_ack_ms=" << static_cast<double>(cancel_ack_ns - cancel_send_ns) / 1000000.0
                     << " hl_resting_ms=" << ((cancel_send_ns > perf_trace_.hl_ack_ns)
                            ? static_cast<double>(cancel_send_ns - perf_trace_.hl_ack_ns) / 1000000.0 : 0.0);
            events.push_back(EventLog {.message = perf_msg.str()});
            
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
            if (ack.ok && ack.fill_confirmed) {
                strategy_.on_lighter_hedge_fill(action.hedge_intent->limit_price);
                const auto& pos = strategy_.open_position();
                msg << "TRADE COMPLETE: lighter hedge CONFIRMED tx=" << ack.tx_hash
                    << " confirmed_sz=" << ack.confirmed_size
                    << " hl_px=" << (pos ? pos->hl_fill_price : 0.0)
                    << " lt_px=" << (pos ? pos->lighter_fill_price : 0.0)
                    << " spread=" << snapshot.cross_spread_bps;
                strategy_.reset();
            } else {
                // Hedge failed or unconfirmed — trigger HL unwind
                const auto reject_logs = execute_action(strategy_.on_lighter_hedge_reject(), snapshot);
                for (const auto& rl : reject_logs) {
                    events.push_back(rl);
                }
                msg << "lighter hedge " << (ack.ok ? "UNCONFIRMED" : "FAILED") << ": " << ack.message
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

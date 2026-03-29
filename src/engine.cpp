#include "arb/engine.hpp"
#include "arb/perf.hpp"

#include <chrono>
#include <cmath>
#include <sstream>

namespace arb {

namespace {
std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
}  // namespace

MakerHedgeEngine::MakerHedgeEngine(
    EngineConfig config,
    HyperliquidExchange& hl,
    LighterExchange& lighter,
    TradeJournal* journal
) : config_(std::move(config)),
    hl_(hl),
    lighter_(lighter),
    strategy_(config_.strategy),
    journal_(journal) {}

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

std::int64_t MakerHedgeEngine::steady_now_ms() const noexcept {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

bool MakerHedgeEngine::position_limit_reached(double mid_price) const noexcept {
    if (mid_price <= 0.0) return false;
    const double pos_usd = std::abs(hl_position_base_) * mid_price;
    return pos_usd >= config_.strategy.max_position_usd;
}

std::vector<EventLog> MakerHedgeEngine::on_market_data(std::int64_t now_ms) {
    const SpreadSnapshot snapshot = collect_snapshot();
    if (deferred_hl_action_.has_value()) {
        auto deferred_logs = execute_deferred_hl_action(now_ms, snapshot);
        if (!deferred_logs.empty()) {
            return deferred_logs;
        }
    }
    PerfCollector::instance().record_hot_path(
        PerfMetric::CrossVenueAlignmentMs,
        static_cast<std::uint64_t>(std::llabs(snapshot.hl.quote_age_ms - snapshot.lighter.quote_age_ms))
    );
    
    // Position limit: if at max, only allow orders that REDUCE the position.
    // pos > 0 (long HL) → only allow sell (ShortLighterLongHl has is_buy=true on HL, WRONG)
    // Actually: check which direction would increase vs decrease position.
    // hl_position_base_ > 0 → long on HL → reduce = sell on HL → LongLighterShortHl
    // hl_position_base_ < 0 → short on HL → reduce = buy on HL → ShortLighterLongHl
    const bool pos_limit_hit = position_limit_reached(snapshot.hl.mid());
    
    const std::uint64_t decision_start_ns = perf_now_ns();
    const Action action = strategy_.on_market_snapshot(snapshot, now_ms, hl_position_base_);
    const std::uint64_t decision_end_ns = perf_now_ns();
    PerfCollector::instance().record_hot_path(
        PerfMetric::StrategyDecisionNs,
        decision_end_ns - decision_start_ns
    );

    if (action.type == ActionType::PlaceHlMaker) {
        // Position limit: block orders that would INCREASE position beyond limit
        if (pos_limit_hit && action.maker_order.has_value()) {
            const bool would_buy_hl = action.maker_order->is_buy;
            const bool would_increase = (hl_position_base_ >= 0.0 && would_buy_hl)
                                     || (hl_position_base_ < 0.0 && !would_buy_hl);
            if (would_increase) {
                strategy_.reset();  // back to Idle so we don't get stuck
                return {};
            }
        }
        perf_trace_ = {};
        perf_trace_.signal_ns = decision_end_ns;
    } else if (action.type == ActionType::CancelHlMaker) {
        perf_trace_.cancel_trigger_ns = decision_end_ns;
    }
    return execute_action(action, snapshot);
}

std::vector<EventLog> MakerHedgeEngine::on_hl_fill(double fill_price, double fill_size_base, const SpreadSnapshot& snapshot, const std::string& oid, std::uint64_t fill_rx_ns, double hl_fee) {
    // BUG FIX 3: Check if this fill belongs to any recently placed order, not just the current active one.
    if (recently_placed_oids_.find(oid) == recently_placed_oids_.end()) {
        return {};
    }
    
    // Do NOT erase the oid here — partial fills arrive as multiple messages
    // with the same oid. Each partial fill must be hedged independently.
    // The oid will be cleaned up when a new order is placed (clear + insert).

    std::vector<EventLog> events;
    
    // SPECULATIVE HEDGE RECONCILIATION
    if (speculative_hedge_sent_ && oid == speculative_hedge_oid_) {
        // Speculative hedge was already sent - reconcile sizes
        const double size_diff = fill_size_base - speculative_hedge_size_;
        
        if (std::abs(size_diff) < 0.001) {
            // Perfect match - no additional hedging needed
            std::ostringstream msg;
            msg << "SPECULATIVE HEDGE RECONCILED: hl_fill=" << fill_size_base 
                << " hedge=" << speculative_hedge_size_ << " diff=0";
            events.push_back(EventLog{.message = msg.str()});
            
            // Update position and reset speculative state
            const Direction dir = last_maker_direction_;
            const bool hl_is_buy = (dir == Direction::ShortLighterLongHl);
            hl_position_base_ += hl_is_buy ? fill_size_base : -fill_size_base;
            
            speculative_hedge_sent_ = false;
            speculative_hedge_size_ = 0.0;
            speculative_hedge_oid_.clear();
            recently_placed_oids_.erase(oid);
            active_hl_oid_.reset();
            strategy_.reset();
            perf_trace_ = {};
            
            return events;
        } else if (size_diff > 0.001) {
            // HL fill is larger - need additional hedge for the difference
            const Direction dir = last_maker_direction_;
            const bool is_ask = (dir == Direction::ShortLighterLongHl);
            const double lighter_mid = snapshot.lighter.mid();
            constexpr double kSlippageBps = 10.0;
            const double hedge_price = is_ask
                ? lighter_mid * (1.0 - kSlippageBps / 10000.0)
                : lighter_mid * (1.0 + kSlippageBps / 10000.0);

            const LighterIocAck ack = lighter_.place_ioc_order(LighterIocRequest {
                .is_ask = is_ask,
                .price = hedge_price,
                .size = size_diff,
                .signal_price = lighter_mid,
                .dry_run = config_.dry_run,
            });

            std::ostringstream msg;
            msg << "SPECULATIVE HEDGE + CORRECTION: hl_fill=" << fill_size_base 
                << " spec_hedge=" << speculative_hedge_size_ 
                << " correction=" << size_diff << " " << (ack.ok ? "SUCCESS" : "FAILED");
            events.push_back(EventLog{.message = msg.str()});
            
            // Update position
            const bool hl_is_buy = (dir == Direction::ShortLighterLongHl);
            hl_position_base_ += hl_is_buy ? fill_size_base : -fill_size_base;
            recently_placed_oids_.erase(oid);
            active_hl_oid_.reset();
            strategy_.reset();
            perf_trace_ = {};
        } else {
            // HL fill is smaller than speculative hedge — unwind the overshoot on Lighter
            const double overshoot = -size_diff;
            const Direction dir = last_maker_direction_;
            const bool spec_was_ask = (dir == Direction::ShortLighterLongHl);
            const double lighter_mid = snapshot.lighter.mid();
            constexpr double kSlippageBps = 15.0;
            // Reverse direction to unwind
            const double unwind_price = spec_was_ask
                ? lighter_mid * (1.0 + kSlippageBps / 10000.0)
                : lighter_mid * (1.0 - kSlippageBps / 10000.0);

            const LighterIocAck unwind_ack = lighter_.place_ioc_order(LighterIocRequest {
                .is_ask = !spec_was_ask,
                .price = unwind_price,
                .size = overshoot,
                .signal_price = lighter_mid,
                .dry_run = config_.dry_run,
            });

            std::ostringstream msg;
            msg << "SPECULATIVE HEDGE OVERSHOOT UNWIND: hl_fill=" << fill_size_base 
                << " spec_hedge=" << speculative_hedge_size_ << " overshoot=" << overshoot
                << " unwind " << (unwind_ack.ok ? "SUCCESS" : "FAILED: " + unwind_ack.message);
            events.push_back(EventLog{.message = msg.str()});
            
            // Update position based on actual HL fill
            const bool hl_is_buy = (dir == Direction::ShortLighterLongHl);
            hl_position_base_ += hl_is_buy ? fill_size_base : -fill_size_base;
        }
        
        // Reset speculative state
        speculative_hedge_sent_ = false;
        speculative_hedge_size_ = 0.0;
        speculative_hedge_oid_.clear();
        
        return events;
    }
    
    // NO SPECULATIVE HEDGE - proceed with normal hedging
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
        .signal_price = lighter_mid,
        .dry_run = config_.dry_run,
    });
    const std::uint64_t lighter_ack_ns = perf_now_ns();
    perf_trace_.lighter_ack_ns = lighter_ack_ns;
    PerfCollector::instance().record_trade_path(
        PerfMetric::LighterSendToAckNs,
        lighter_ack_ns - lighter_send_ns
    );
    PerfCollector::instance().record_trade_path(
        PerfMetric::HlFillToLighterHttpAckTotalNs,
        static_cast<std::uint64_t>(std::llround((ack.place_to_http_ack_latency_ms * 1000000.0) + static_cast<double>(lighter_send_ns - fill_rx_ns)))
    );
    PerfCollector::instance().record_trade_path(
        PerfMetric::MakerFillToTakerAckTotalNs,
        lighter_ack_ns - fill_rx_ns
    );

    std::ostringstream msg;
    std::ostringstream perf_msg;
    if (ack.ok && ack.fill_confirmed) {
        msg << "TRADE COMPLETE: hl_px=" << fill_price << " sz=" << fill_size_base
            << " lt_fill_px=" << ack.fill_price << " lt_sz=" << ack.confirmed_size
            << " lt_hedge tx=" << ack.tx_hash
            << " spread=" << snapshot.cross_spread_bps
            << " hl_fee=" << hl_fee;
        // Track HL position: buy adds, sell subtracts
        const bool hl_is_buy = (dir == Direction::ShortLighterLongHl);
        hl_position_base_ += hl_is_buy ? fill_size_base : -fill_size_base;
        perf_msg << "perf trade oid=" << oid
                 << " signal_to_hl_send_ms=" << ((perf_trace_.hl_send_ns > perf_trace_.signal_ns)
                        ? static_cast<double>(perf_trace_.hl_send_ns - perf_trace_.signal_ns) / 1000000.0 : 0.0)
                 << " hl_send_to_ack_ms=" << ((perf_trace_.hl_ack_ns > perf_trace_.hl_send_ns)
                        ? static_cast<double>(perf_trace_.hl_ack_ns - perf_trace_.hl_send_ns) / 1000000.0 : 0.0)
                 << " hl_sign_order_ms=" << perf_trace_.hl_sign_order_ms
                 << " hl_ws_send_call_ms=" << perf_trace_.hl_ws_send_call_ms
                 << " hl_ws_send_to_response_rx_ms=" << perf_trace_.hl_ws_send_to_response_rx_ms
                 << " hl_response_rx_to_unblock_ms=" << perf_trace_.hl_response_rx_to_unblock_ms
                 << " hl_resting_ms=" << ((fill_rx_ns > perf_trace_.hl_ack_ns)
                        ? static_cast<double>(fill_rx_ns - perf_trace_.hl_ack_ns) / 1000000.0 : 0.0)
                 << " hl_fill_rx_to_lt_send_ms=" << static_cast<double>(lighter_send_ns - fill_rx_ns) / 1000000.0
                 << " hl_fill_to_lt_http_ack_ms=" << (static_cast<double>(lighter_send_ns - fill_rx_ns) / 1000000.0 + ack.place_to_http_ack_latency_ms)
                 << " lt_nonce_fetch_ms=" << ack.nonce_fetch_latency_ms
                 << " lt_sign_order_ms=" << ack.sign_order_latency_ms
                 << " lt_sendtx_ack_ms=" << ack.send_tx_ack_latency_ms
                 << " lt_send_to_http_ack_ms=" << ack.http_ack_latency_ms
                 << " lt_http_ack_to_fill_confirm_ms=" << ack.fill_confirm_latency_ms
                 << " lt_confirm_attempts=" << ack.confirm_attempts
                 << " lt_send_to_ack_ms=" << static_cast<double>(lighter_ack_ns - lighter_send_ns) / 1000000.0
                 << " hedge_total_ms=" << static_cast<double>(lighter_ack_ns - fill_rx_ns) / 1000000.0;
        if (journal_) {
            const double maker_resting = (fill_rx_ns > perf_trace_.hl_ack_ns && perf_trace_.hl_ack_ns > 0)
                ? static_cast<double>(fill_rx_ns - perf_trace_.hl_ack_ns) / 1000000.0 : 0.0;
            const double hedge_total = static_cast<double>(lighter_ack_ns - fill_rx_ns) / 1000000.0;
            journal_->record(JournalEntry {
                .trade_id = now_us(),
                .timestamp_us = now_us(),
                .type = 'T',
                .hedge_status = "filled",
                .hl_fill_timestamp_us = static_cast<std::int64_t>(fill_rx_ns / 1000),
                .lighter_fill_timestamp_us = static_cast<std::int64_t>(lighter_ack_ns / 1000),
                .hl_side = hl_is_buy ? 'B' : 'S',
                .hl_px = fill_price,
                .hl_sz = fill_size_base,
                .hl_fee = hl_fee,
                .lt_fill_px = ack.fill_price,
                .lt_sz = ack.confirmed_size,
                .lt_fee = ack.fee,
                .spread_bps = snapshot.cross_spread_bps,
                .signal_spread_bps = 0.0,
                .maker_resting_ms = maker_resting,
                .hedge_total_ms = hedge_total,
                .hl_oid = oid,
                .lt_tx = ack.tx_hash,
            });
        }
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
                    << " hl_fill_to_lt_http_ack_ms=" << (static_cast<double>(lighter_send_ns - fill_rx_ns) / 1000000.0 + ack.place_to_http_ack_latency_ms)
                    << " lt_nonce_fetch_ms=" << ack.nonce_fetch_latency_ms
                    << " lt_sign_order_ms=" << ack.sign_order_latency_ms
                    << " lt_sendtx_ack_ms=" << ack.send_tx_ack_latency_ms
                    << " lt_send_to_http_ack_ms=" << ack.http_ack_latency_ms
                    << " lt_http_ack_to_fill_confirm_ms=" << ack.fill_confirm_latency_ms
                    << " lt_confirm_attempts=" << ack.confirm_attempts
                    << " lt_send_to_ack_ms=" << static_cast<double>(lighter_ack_ns - lighter_send_ns) / 1000000.0
                    << " hedge_total_ms=" << static_cast<double>(lighter_ack_ns - fill_rx_ns) / 1000000.0
                    << " hedge_fail_to_unwind_send_ms=" << static_cast<double>(unwind_send_ns - lighter_ack_ns) / 1000000.0
                    << " unwind_send_to_ack_ms=" << static_cast<double>(unwind_ack_ns - unwind_send_ns) / 1000000.0;
        events.push_back(EventLog {.message = unwind_perf.str()});
        if (journal_) {
            journal_->record(JournalEntry {
                .trade_id = now_us(),
                .timestamp_us = now_us(),
                .type = 'U',
                .hedge_status = "unconfirmed_then_unwind",
                .hl_fill_timestamp_us = static_cast<std::int64_t>(fill_rx_ns / 1000),
                .hl_side = is_ask ? 'S' : 'B',
                .hl_px = fill_price,
                .hl_sz = fill_size_base,
                .hl_fee = hl_fee,
                .spread_bps = snapshot.cross_spread_bps,
                .hedge_total_ms = static_cast<double>(lighter_ack_ns - fill_rx_ns) / 1000000.0,
                .hl_oid = oid,
                .lt_tx = ack.tx_hash,
                .unwind_fill_px = unwind.avg_fill_price,
                .unwind_fill_sz = unwind.filled_size,
                .failure_reason = "lighter_unconfirmed",
            });
        }
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
                    << " hl_fill_to_lt_http_ack_ms=" << (static_cast<double>(lighter_send_ns - fill_rx_ns) / 1000000.0 + ack.place_to_http_ack_latency_ms)
                    << " lt_nonce_fetch_ms=" << ack.nonce_fetch_latency_ms
                    << " lt_sign_order_ms=" << ack.sign_order_latency_ms
                    << " lt_sendtx_ack_ms=" << ack.send_tx_ack_latency_ms
                    << " lt_send_to_http_ack_ms=" << ack.http_ack_latency_ms
                    << " lt_http_ack_to_fill_confirm_ms=" << ack.fill_confirm_latency_ms
                    << " lt_confirm_attempts=" << ack.confirm_attempts
                    << " lt_send_to_ack_ms=" << static_cast<double>(lighter_ack_ns - lighter_send_ns) / 1000000.0
                    << " hedge_total_ms=" << static_cast<double>(lighter_ack_ns - fill_rx_ns) / 1000000.0
                    << " hedge_fail_to_unwind_send_ms=" << static_cast<double>(unwind_send_ns - lighter_ack_ns) / 1000000.0
                    << " unwind_send_to_ack_ms=" << static_cast<double>(unwind_ack_ns - unwind_send_ns) / 1000000.0;
        events.push_back(EventLog {.message = unwind_perf.str()});
        if (journal_) {
            journal_->record(JournalEntry {
                .trade_id = now_us(),
                .timestamp_us = now_us(),
                .type = 'F',
                .hedge_status = "failed",
                .hl_fill_timestamp_us = static_cast<std::int64_t>(fill_rx_ns / 1000),
                .hl_side = is_ask ? 'S' : 'B',
                .hl_px = fill_price,
                .hl_sz = fill_size_base,
                .hl_fee = hl_fee,
                .spread_bps = snapshot.cross_spread_bps,
                .hedge_total_ms = static_cast<double>(lighter_ack_ns - fill_rx_ns) / 1000000.0,
                .hl_oid = oid,
                .unwind_fill_px = unwind.avg_fill_price,
                .unwind_fill_sz = unwind.filled_size,
                .failure_reason = ack.message.substr(0, 100),
            });
        }
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

std::optional<std::int64_t> MakerHedgeEngine::next_retry_steady_ms() const noexcept {
    return next_retry_steady_ms_;
}

void MakerHedgeEngine::set_hl_position(double base_size) noexcept {
    hl_position_base_ = base_size;
}

std::vector<EventLog> MakerHedgeEngine::execute_action(const Action& action, const SpreadSnapshot& snapshot) {
    std::vector<EventLog> events;

    switch (action.type) {
        case ActionType::None:
            return events;
        case ActionType::PlaceHlMaker: {
            // Position limit: block orders that would increase position beyond max
            if (action.maker_order) {
                const double mid = snapshot.hl.mid();
                if (mid > 0.0) {
                    const double pos_usd = std::abs(hl_position_base_) * mid;
                    if (pos_usd >= config_.strategy.max_position_usd) {
                        const bool would_buy_hl = action.maker_order->is_buy;
                        const bool would_increase = (hl_position_base_ >= 0.0 && would_buy_hl)
                                                 || (hl_position_base_ < 0.0 && !would_buy_hl);
                        if (would_increase) {
                            strategy_.reset();  // back to Idle, don't get stuck in PendingHlMaker
                            return events;
                        }
                    }
                }
            }
            {
                const auto now = steady_now_ms();
                if (const auto retry_at = hl_place_retry_at_ms(now); retry_at.has_value()) {
                    deferred_hl_action_ = DeferredHlAction {.action = action};
                    next_retry_steady_ms_ = retry_at;
                    return events;
                }
                last_hl_place_ms_ = now;
                next_retry_steady_ms_.reset();
                deferred_hl_action_.reset();
            }
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
            perf_trace_.hl_sign_order_ms = ack.sign_latency_ms;
            perf_trace_.hl_ws_send_call_ms = ack.ws_send_call_latency_ms;
            perf_trace_.hl_ws_send_to_response_rx_ms = ack.ws_send_to_response_rx_latency_ms;
            perf_trace_.hl_response_rx_to_unblock_ms = ack.response_rx_to_unblock_latency_ms;
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
                // Record wall-clock time for trade timestamp filtering
                maker_order_placed_epoch_ms_ = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                // Reset speculative hedge state for new order
                speculative_hedge_sent_ = false;
                speculative_hedge_size_ = 0.0;
                speculative_hedge_oid_.clear();
                events.push_back(EventLog {.message = "placed hl maker oid=" + ack.oid});
            } else {
                events.push_back(EventLog {.message = "hl maker placement failed: " + ack.message});
            }
            return events;
        }
        case ActionType::CancelHlMaker: {
            {
                const auto now = steady_now_ms();
                if (const auto retry_at = hl_cancel_retry_at_ms(now); retry_at.has_value()) {
                    deferred_hl_action_ = DeferredHlAction {.action = action};
                    next_retry_steady_ms_ = retry_at;
                    return events;
                }
                last_hl_cancel_ms_ = now;
                next_retry_steady_ms_.reset();
                deferred_hl_action_.reset();
            }
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
            
            if (ack.ok) {
                // Cancel succeeded — safe to unwind speculative hedge and clear OID
                if (speculative_hedge_sent_) {
                    const Direction dir = last_maker_direction_;
                    const bool spec_was_ask = (dir == Direction::ShortLighterLongHl);
                    const double lighter_mid = snapshot.lighter.mid();
                    constexpr double kSlippageBps = 15.0;
                    const double unwind_price = spec_was_ask
                        ? lighter_mid * (1.0 + kSlippageBps / 10000.0)
                        : lighter_mid * (1.0 - kSlippageBps / 10000.0);

                    const LighterIocAck unwind_ack = lighter_.place_ioc_order(LighterIocRequest {
                        .is_ask = !spec_was_ask,
                        .price = unwind_price,
                        .size = speculative_hedge_size_,
                        .signal_price = lighter_mid,
                        .dry_run = config_.dry_run,
                    });

                    std::ostringstream unwind_msg;
                    unwind_msg << "SPECULATIVE HEDGE UNWIND (HL cancelled): sz=" << speculative_hedge_size_
                               << " " << (unwind_ack.ok ? "SUCCESS" : "FAILED: " + unwind_ack.message);
                    events.push_back(EventLog{.message = unwind_msg.str()});
                }
                speculative_hedge_sent_ = false;
                speculative_hedge_size_ = 0.0;
                speculative_hedge_oid_.clear();
                active_hl_oid_.reset();
                strategy_.reset();
            } else {
                // Cancel failed — order may still be live or already filled.
                // Do NOT clear active_hl_oid_ or unwind speculative hedge.
                // The fill path will handle reconciliation if it was filled.
                events.push_back(EventLog{.message = "cancel failed, keeping OID active for fill reconciliation"});
            }
            return events;
        }
        case ActionType::SendLighterTakerHedge: {
            // Rate limit Lighter API (but DON'T skip hedges — they're critical)
            last_lighter_api_ms_ = steady_now_ms();
            const LighterIocAck ack = lighter_.place_ioc_order(LighterIocRequest {
                .is_ask = action.hedge_intent->is_ask,
                .price = action.hedge_intent->limit_price,
                .size = action.hedge_intent->size_base,
                .signal_price = action.hedge_intent->limit_price,
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

std::vector<EventLog> MakerHedgeEngine::execute_deferred_hl_action(std::int64_t now_ms, const SpreadSnapshot& snapshot) {
    if (!deferred_hl_action_.has_value()) {
        return {};
    }
    if (next_retry_steady_ms_.has_value() && now_ms < *next_retry_steady_ms_) {
        return {};
    }

    const Action action = deferred_hl_action_->action;
    deferred_hl_action_.reset();
    next_retry_steady_ms_.reset();
    return execute_action(action, snapshot);
}

std::optional<std::int64_t> MakerHedgeEngine::hl_place_retry_at_ms(std::int64_t now_ms) const noexcept {
    const std::int64_t elapsed = now_ms - last_hl_place_ms_;
    if (elapsed >= config_.hl_order_interval_ms) {
        return std::nullopt;
    }
    return last_hl_place_ms_ + config_.hl_order_interval_ms;
}

std::optional<std::int64_t> MakerHedgeEngine::hl_cancel_retry_at_ms(std::int64_t now_ms) const noexcept {
    const std::int64_t elapsed = now_ms - last_hl_cancel_ms_;
    if (elapsed >= config_.hl_order_interval_ms) {
        return std::nullopt;
    }
    return last_hl_cancel_ms_ + config_.hl_order_interval_ms;
}

std::vector<EventLog> MakerHedgeEngine::on_trade_event(const TradeEvent& trade) {
    // Only react to trades when we have an active maker order
    if (!active_hl_oid_.has_value()) {
        return {};
    }

    const auto& strategy_pending = strategy_.pending_maker();
    if (!strategy_pending.has_value()) {
        return {};
    }

    // Skip if speculative hedge already sent for this order
    if (speculative_hedge_sent_) {
        return {};
    }

    // TIMESTAMP GATE: only react to trades that occurred AFTER our maker order
    // was placed. Stale/delayed trades must not trigger speculative hedges.
    if (trade.exchange_time_ms > 0 && maker_order_placed_epoch_ms_ > 0
        && trade.exchange_time_ms < maker_order_placed_epoch_ms_) {
        return {};  // trade is older than our order placement
    }

    const double trade_price = trade.price;
    const double maker_price = strategy_pending->price;
    const bool maker_is_buy = strategy_pending->is_buy;

    // Check if trade price crosses our maker order price with minimum depth
    const double min_cross_bps = config_.spec_hedge_min_cross_bps;
    bool price_crossed = false;
    if (maker_is_buy) {
        const double threshold = maker_price * (1.0 - min_cross_bps / 10000.0);
        if (trade_price <= threshold) {
            price_crossed = true; // Trade sufficiently below our buy order
        }
    } else {
        const double threshold = maker_price * (1.0 + min_cross_bps / 10000.0);
        if (trade_price >= threshold) {
            price_crossed = true; // Trade sufficiently above our sell order
        }
    }

    // Filter by trade size relative to our order size
    const double min_ratio = config_.spec_hedge_min_trade_ratio;
    if (price_crossed && min_ratio > 0.0) {
        const double order_size = strategy_pending->size_base;
        if (order_size > 0.0 && trade.size / order_size < min_ratio) {
            price_crossed = false; // Trade too small, likely not our fill
        }
    }

    if (!price_crossed) {
        // Log periodically for debugging (every 100th non-crossing trade while order is active)
        static int skip_count = 0;
        if (++skip_count % 100 == 0) {
            std::vector<EventLog> dbg;
            std::ostringstream m;
            m << "spec_hedge_debug: " << skip_count << " trades checked, none crossed. "
              << "maker_px=" << maker_price << " " << (maker_is_buy ? "BUY" : "SELL")
              << " last_trade_px=" << trade_price;
            dbg.push_back(EventLog{.message = m.str()});
            return dbg;
        }
        return {};
    }

    // Get current market snapshot for hedge pricing
    const SpreadSnapshot snapshot = collect_snapshot();
    if (!snapshot.valid(config_.strategy.max_quote_age_ms)) {
        return {};
    }

    // Fire speculative hedge immediately
    const Direction dir = strategy_pending->direction;
    const bool is_ask = (dir == Direction::ShortLighterLongHl);
    const double hedge_size = strategy_pending->size_base;

    // Use aggressive pricing to ensure fill
    const double lighter_mid = snapshot.lighter.mid();
    constexpr double kSlippageBps = 15.0; // More aggressive than normal hedge
    const double hedge_price = is_ask
        ? lighter_mid * (1.0 - kSlippageBps / 10000.0)
        : lighter_mid * (1.0 + kSlippageBps / 10000.0);

    const std::uint64_t hedge_send_ns = perf_now_ns();
    const LighterIocAck ack = lighter_.place_ioc_order(LighterIocRequest {
        .is_ask = is_ask,
        .price = hedge_price,
        .size = hedge_size,
        .signal_price = lighter_mid,
        .dry_run = config_.dry_run,
    });
    const std::uint64_t hedge_ack_ns = perf_now_ns();

    // Mark speculative hedge as sent
    speculative_hedge_sent_ = true;
    speculative_hedge_size_ = hedge_size;
    speculative_hedge_oid_ = *active_hl_oid_;

    std::vector<EventLog> events;
    std::ostringstream msg;
    
    if (ack.ok && ack.fill_confirmed) {
        msg << "SPECULATIVE HEDGE SUCCESS: trade_px=" << trade_price
            << " maker_px=" << maker_price << " hedge_px=" << ack.fill_price
            << " hedge_sz=" << ack.confirmed_size << " tx=" << ack.tx_hash
            << " latency_ms=" << static_cast<double>(hedge_ack_ns - hedge_send_ns) / 1000000.0;
    } else {
        msg << "SPECULATIVE HEDGE FAILED: trade_px=" << trade_price
            << " maker_px=" << maker_price << " err=" << ack.message
            << " latency_ms=" << static_cast<double>(hedge_ack_ns - hedge_send_ns) / 1000000.0;
        // Reset flag since hedge failed
        speculative_hedge_sent_ = false;
        speculative_hedge_size_ = 0.0;
        speculative_hedge_oid_.clear();
    }

    events.push_back(EventLog{.message = msg.str()});
    return events;
}

}  // namespace arb

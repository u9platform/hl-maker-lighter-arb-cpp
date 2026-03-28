#pragma once

#include "arb/exchange.hpp"
#include "arb/journal.hpp"
#include "arb/strategy.hpp"

#include <memory>
#include <optional>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace arb {

struct EngineConfig {
    StrategyConfig strategy;
    std::string hl_coin {"HYPE"};
    std::int64_t lighter_market_id {24};
    bool dry_run {true};
    std::int64_t hl_order_interval_ms {200};     // Min ms between HL place/cancel API calls
    std::int64_t lighter_order_interval_ms {500}; // Min ms between Lighter hedge API calls
};

struct EventLog {
    std::string message;
};

class MakerHedgeEngine {
  public:
    MakerHedgeEngine(
        EngineConfig config,
        HyperliquidExchange& hl,
        LighterExchange& lighter,
        TradeJournal* journal = nullptr
    );

    [[nodiscard]] SpreadSnapshot collect_snapshot() const;
    [[nodiscard]] std::vector<EventLog> on_market_data(std::int64_t now_ms);
    [[nodiscard]] std::vector<EventLog> on_hl_fill(double fill_price, double fill_size_base, const SpreadSnapshot& snapshot, const std::string& oid, std::uint64_t fill_rx_ns, double hl_fee = 0.0);
    [[nodiscard]] std::vector<EventLog> on_lighter_hedge_reject();
    void on_lighter_hedge_fill(double fill_price);

    /// Speculative hedge when trade price crosses our maker order.
    [[nodiscard]] std::vector<EventLog> on_trade_event(const TradeEvent& trade);

    [[nodiscard]] const std::optional<std::string>& active_hl_oid() const noexcept;
    [[nodiscard]] const HlMakerLighterHedger& strategy() const noexcept;
    [[nodiscard]] double hl_position_base() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> next_retry_steady_ms() const noexcept;
    void set_hl_position(double base_size) noexcept;

  private:
    struct OrderPerfTrace {
        std::string oid;
        std::uint64_t signal_ns {0};
        std::uint64_t hl_send_ns {0};
        std::uint64_t hl_ack_ns {0};
        double hl_sign_order_ms {0.0};
        double hl_ws_send_call_ms {0.0};
        double hl_ws_send_to_response_rx_ms {0.0};
        double hl_response_rx_to_unblock_ms {0.0};
        std::uint64_t cancel_trigger_ns {0};
        std::uint64_t cancel_send_ns {0};
        std::uint64_t hl_fill_rx_ns {0};
        std::uint64_t lighter_send_ns {0};
        std::uint64_t lighter_ack_ns {0};
    };

    [[nodiscard]] std::vector<EventLog> execute_action(const Action& action, const SpreadSnapshot& snapshot);
    [[nodiscard]] std::vector<EventLog> execute_deferred_hl_action(std::int64_t now_ms, const SpreadSnapshot& snapshot);
    [[nodiscard]] std::optional<std::int64_t> hl_place_retry_at_ms(std::int64_t now_ms) const noexcept;
    [[nodiscard]] std::optional<std::int64_t> hl_cancel_retry_at_ms(std::int64_t now_ms) const noexcept;

    EngineConfig config_;
    HyperliquidExchange& hl_;
    LighterExchange& lighter_;
    HlMakerLighterHedger strategy_;
    std::optional<std::string> active_hl_oid_;
    
    // Track recently placed OIDs to handle race condition between
    // cancel and fill messages, and partial fills with same oid.
    std::unordered_set<std::string> recently_placed_oids_;
    
    // Remember the direction of the last maker order so partial fills
    // can hedge correctly even after strategy state is reset.
    Direction last_maker_direction_ {Direction::ShortLighterLongHl};
    
    // Track absolute HL position in base units for position limit enforcement.
    // Positive = long, negative = short. Updated on each fill.
    double hl_position_base_ {0.0};
    OrderPerfTrace perf_trace_;

    // Speculative hedge tracking
    bool speculative_hedge_sent_ {false};
    double speculative_hedge_size_ {0.0};
    std::string speculative_hedge_oid_;
    std::uint64_t maker_order_placed_epoch_ms_ {0}; // epoch ms when HL maker order was acked

    // Rate limiting: track last API call timestamps (steady_clock ms)
    struct DeferredHlAction {
        Action action;
    };
    std::int64_t last_hl_place_ms_ {0};
    std::int64_t last_hl_cancel_ms_ {0};
    std::int64_t last_lighter_api_ms_ {0};
    std::optional<DeferredHlAction> deferred_hl_action_;
    std::optional<std::int64_t> next_retry_steady_ms_;
    [[nodiscard]] std::int64_t steady_now_ms() const noexcept;
    
    // Returns true if current position exceeds max_position_usd.
    [[nodiscard]] bool position_limit_reached(double mid_price) const noexcept;

    // Trade journal — optional, owned externally.  nullptr disables.
    TradeJournal* journal_ {nullptr};
};

}  // namespace arb

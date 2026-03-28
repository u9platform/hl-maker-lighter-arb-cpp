#pragma once

#include "arb/types.hpp"
#include "arb/ws_client.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace arb {

/// Thread-safe BBO store updated by WS callbacks.
/// Readers on the strategy thread call latest_snapshot().
/// Writers are the WS IO threads.
struct AtomicBbo {
    mutable std::mutex mu;
    Bbo bbo;
    std::chrono::steady_clock::time_point last_update {};

    void store(double bid, double ask) {
        std::lock_guard lock(mu);
        bbo.bid = bid;
        bbo.ask = ask;
        last_update = std::chrono::steady_clock::now();
    }

    [[nodiscard]] Bbo load() const {
        std::lock_guard lock(mu);
        Bbo out = bbo;
        if (last_update.time_since_epoch().count() > 0) {
            const auto age = std::chrono::steady_clock::now() - last_update;
            out.quote_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(age).count();
        } else {
            out.quote_age_ms = 999999;
        }
        return out;
    }
};

/// Callback fired on *every* BBO update from either venue.
using OnBboUpdate = std::function<void()>;

/// Callback fired on HL trade events (for speculative hedging).
using OnTradeCallback = std::function<void(const TradeEvent&)>;

/// Manages WS connections to HL and Lighter, parses orderbook messages,
/// and maintains thread-safe BBO state.
class MarketFeed {
  public:
    struct Config {
        std::string hl_ws_host {"api.hyperliquid.xyz"};
        std::string hl_ws_path {"/ws"};
        std::string hl_coin {"HYPE"};
        std::string lighter_ws_host {"mainnet.zklighter.elliot.ai"};
        std::string lighter_ws_path {"/stream"};
        int lighter_market_id {24};
    };

    explicit MarketFeed(Config config);
    ~MarketFeed();

    MarketFeed(const MarketFeed&) = delete;
    MarketFeed& operator=(const MarketFeed&) = delete;

    /// Set callback before start().
    void set_on_update(OnBboUpdate cb);

    /// Set trade callback for speculative hedging.
    void set_on_trade(OnTradeCallback cb);

    /// Start both WS connections.
    void start();

    /// Stop both WS connections.
    void stop();

    /// Get current snapshot. Thread-safe, called from strategy thread.
    [[nodiscard]] SpreadSnapshot snapshot() const;

    /// Get individual BBOs. Thread-safe.
    [[nodiscard]] Bbo hl_bbo() const;
    [[nodiscard]] Bbo lighter_bbo() const;

    [[nodiscard]] bool hl_connected() const noexcept;
    [[nodiscard]] bool lighter_connected() const noexcept;

    /// Access internal BBO stores (for WS exchange adapters).
    [[nodiscard]] AtomicBbo& hl_bbo_store() noexcept { return hl_bbo_; }
    [[nodiscard]] AtomicBbo& lighter_bbo_store() noexcept { return lighter_bbo_; }

  private:
    void on_hl_message(const std::string& msg);
    void on_lighter_message(const std::string& msg);
    void subscribe_hl();
    void subscribe_lighter();

    Config config_;
    std::unique_ptr<WsClient> hl_ws_;
    std::unique_ptr<WsClient> lighter_ws_;
    AtomicBbo hl_bbo_;
    AtomicBbo lighter_bbo_;
    OnBboUpdate on_update_;
    OnTradeCallback on_trade_;
    std::atomic<bool> hl_subscribed_ {false};
    std::atomic<bool> lighter_subscribed_ {false};
    std::atomic<bool> hl_trades_subscribed_ {false};
};

/// HL WebSocket user fill feed — subscribe to userFills for fill callbacks.
class HlFillFeed {
  public:
    struct Config {
        std::string ws_host {"api.hyperliquid.xyz"};
        std::string ws_path {"/ws"};
        std::string user_address;
    };

    using FillCallback = std::function<void(const std::string& coin, double price, double size, bool is_buy, const std::string& oid, double fee)>;
    using DisconnectCallback = std::function<void(const std::string& reason)>;

    explicit HlFillFeed(Config config);
    ~HlFillFeed();

    void set_on_fill(FillCallback cb);
    void set_on_disconnect(DisconnectCallback cb);
    void start();
    void stop();
    [[nodiscard]] bool is_connected() const noexcept;
    [[nodiscard]] bool is_subscribed() const noexcept;

  private:
    void on_message(const std::string& msg);
    void subscribe();

    Config config_;
    std::unique_ptr<WsClient> ws_;
    FillCallback on_fill_;
    DisconnectCallback on_disconnect_;
    std::atomic<bool> subscribed_ {false};
};

class LighterPositionFeed {
  public:
    struct Config {
        std::string ws_host {"mainnet.zklighter.elliot.ai"};
        std::string ws_path {"/stream"};
        std::int64_t account_index {0};
        int market_index {24};
        std::string auth_token;
    };

    explicit LighterPositionFeed(Config config);
    ~LighterPositionFeed();

    LighterPositionFeed(const LighterPositionFeed&) = delete;
    LighterPositionFeed& operator=(const LighterPositionFeed&) = delete;

    void start();
    void stop();
    [[nodiscard]] bool is_connected() const noexcept;
    [[nodiscard]] bool is_subscribed() const noexcept;
    [[nodiscard]] bool wait_until_connected(int timeout_ms) const;
    [[nodiscard]] std::optional<LighterPositionSnapshot> wait_for_position_change(double baseline_size, int timeout_ms);

  private:
    void on_message(const std::string& msg);
    void subscribe();

    Config config_;
    std::unique_ptr<WsClient> ws_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::optional<LighterPositionSnapshot> latest_;
    std::uint64_t version_ {0};
    std::atomic<bool> subscribed_ {false};
};

}  // namespace arb

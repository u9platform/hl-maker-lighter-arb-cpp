#include "arb/engine.hpp"
#include "arb/hl_ws_post.hpp"
#include "arb/http.hpp"
#include "arb/journal.hpp"
#include "arb/lighter_ws_sendtx.hpp"
#include "arb/market_feed.hpp"
#include "arb/native_trading.hpp"
#include "arb/perf.hpp"
#include "arb/risk.hpp"
#include "arb/types.hpp"
#include "arb/ws_exchange.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

namespace {

// Query HL clearinghouseState for current position of a coin.
// Returns size in base units (negative = short).
double query_hl_position(const std::string& api_url, const std::string& address, const std::string& coin) {
    try {
        const std::string payload = "{\"type\":\"clearinghouseState\",\"user\":\"" + address + "\"}";
        const auto resp = arb::http_post(api_url + "/info", payload, {{"Content-Type", "application/json"}});
        if (resp.status_code != 200) return 0.0;

        // Find the position for our coin using regex on the JSON response
        // Pattern: "coin":"HYPE"...,"szi":"<number>"
        // We need to find the right assetPositions entry
        const std::string& body = resp.body;
        
        // Simple approach: find "coin":"HYPE" then the nearest "szi":"..." 
        std::string coin_pattern = "\"coin\":\"" + coin + "\"";
        auto coin_pos = body.find(coin_pattern);
        if (coin_pos == std::string::npos) return 0.0;
        
        // Find "szi" after the coin match
        auto szi_pos = body.find("\"szi\":\"", coin_pos);
        if (szi_pos == std::string::npos) return 0.0;
        szi_pos += 7; // skip past "szi":"
        auto szi_end = body.find("\"", szi_pos);
        if (szi_end == std::string::npos) return 0.0;
        
        return std::stod(body.substr(szi_pos, szi_end - szi_pos));
    } catch (const std::exception& e) {
        std::cerr << "[main] WARNING: failed to query HL position: " << e.what() << "\n";
        return 0.0;
    }
}

#ifndef ARB_GIT_VERSION
#define ARB_GIT_VERSION "unknown"
#endif

std::atomic<bool> g_running {true};
std::atomic<std::uint64_t> g_event_seq {0};
std::mutex g_cv_mu;
std::condition_variable g_cv;

// Thread-safe fill event queue — WS IO thread pushes, main loop pops.
struct FillEvent {
    std::string coin;
    double price;
    double size;
    bool is_buy;
    std::string oid;
    double fee {0.0};
    std::uint64_t local_rx_ns {0};
};

std::mutex g_fill_mu;
std::queue<FillEvent> g_fill_queue;

void signal_handler(int /*sig*/) {
    g_running.store(false, std::memory_order_release);
    g_cv.notify_all();
}

void publish_event() {
    g_event_seq.fetch_add(1, std::memory_order_release);
    g_cv.notify_one();
}

std::string env_or(const char* name, const std::string& fallback) {
    const char* val = std::getenv(name);
    return val != nullptr ? std::string(val) : fallback;
}

std::string env_required(const char* name) {
    const char* val = std::getenv(name);
    if (val == nullptr || std::string(val).empty()) {
        std::cerr << "ERROR: " << name << " environment variable required\n";
        std::exit(1);
    }
    return val;
}

double env_double(const char* name, double fallback) {
    const char* val = std::getenv(name);
    if (val == nullptr) return fallback;
    try { return std::stod(val); } catch (...) { return fallback; }
}

int env_int(const char* name, int fallback) {
    const char* val = std::getenv(name);
    if (val == nullptr) return fallback;
    try { return std::stoi(val); } catch (...) { return fallback; }
}

std::int64_t current_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string timestamp_str() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&t), "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms;
    return oss.str();
}

}  // namespace

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // --- Config from environment ---
    const std::string hl_private_key = env_required("HL_PRIVATE_KEY");
    const std::string lighter_api_key = env_required("LIGHTER_API_PRIVATE_KEY");
    const int lighter_account_index = env_int("LIGHTER_ACCOUNT_INDEX", 0);
    const int lighter_api_key_index = env_int("LIGHTER_API_KEY_INDEX", 0);
    const std::string hl_user_address = env_or("HL_USER_ADDRESS", "");
    const bool dry_run = env_or("DRY_RUN", "true") == "true";

    const double spread_bps = env_double("SPREAD_BPS", 2.0);
    const double close_spread_bps = env_double("CLOSE_SPREAD_BPS", 0.0);  // 0 = same as spread_bps
    const double cancel_band = env_double("CANCEL_BAND_BPS", 0.5);
    const double pair_size = env_double("PAIR_SIZE_USD", 25.0);
    const double max_pos = env_double("MAX_POSITION_USD", 100.0);

    std::cerr << "=== HL Maker / Lighter Taker Arb (C++) ===\n"
              << "version=" << ARB_GIT_VERSION
              << " "
              << "dry_run=" << (dry_run ? "true" : "false")
              << " spread=" << spread_bps << " close_spread=" << (close_spread_bps > 0.0 ? close_spread_bps : spread_bps) << " cancel_band=" << cancel_band
              << " pair_size=" << pair_size << " max_pos=" << max_pos
              << " hl_interval=" << env_or("HL_ORDER_INTERVAL_MS", "200") << "ms"
              << " lt_interval=" << env_or("LIGHTER_ORDER_INTERVAL_MS", "500") << "ms\n";

    // --- Market Feed (WS for BBO reads) ---
    arb::MarketFeed::Config feed_config;
    feed_config.hl_coin = "HYPE";
    feed_config.lighter_market_id = 24;
    arb::MarketFeed feed(feed_config);

    // --- Native Trading Clients (for order submission) ---
    // Agent wallet mode: the approved agent key signs directly for the main account.
    // vault_address in action_hash and API should be None (not the main account address).
    // HL_USER_ADDRESS is only used for fill feed subscription and position queries.
    arb::NativeHyperliquidTrading hl_native({
        .api_url = "https://api.hyperliquid.xyz",
        .private_key = hl_private_key,
        .vault_address = std::nullopt,
        .coin = "HYPE",
    });
    arb::HlWsPostTransport hl_ws_post;
    hl_ws_post.start();
    if (hl_ws_post.wait_until_connected(2000)) {
        hl_native.set_action_transport([&hl_ws_post](const std::string& payload_json) {
            return hl_ws_post.post_action(payload_json);
        });
        std::cerr << "[main] hl order transport=ws_post\n";
    } else {
        std::cerr << "ERROR: hl ws_post transport unavailable; refusing to fall back to HTTP\n";
        return 1;
    }

    arb::NativeLighterTrading lighter_native({
        .api_url = "https://mainnet.zklighter.elliot.ai",
        .api_private_key = lighter_api_key,
        .account_index = lighter_account_index,
        .api_key_index = lighter_api_key_index,
        .market_index = 24,
    });
    arb::LighterPositionFeed lighter_position_feed({
        .account_index = lighter_account_index,
        .market_index = 24,
        .auth_token = lighter_native.create_auth_token(current_timestamp_ms() + 24LL * 60LL * 60LL * 1000LL),
    });
    lighter_position_feed.start();
    if (lighter_position_feed.wait_until_connected(2000)) {
        lighter_native.set_position_waiter(
            [&lighter_position_feed](double baseline_size, int timeout_ms) {
                return lighter_position_feed.wait_for_position_change(baseline_size, timeout_ms);
            }
        );
        std::cerr << "[main] lighter position confirmation=ws_account_all_positions\n";
    } else {
        std::cerr << "[main] WARNING: lighter position feed unavailable, falling back to direct account query\n";
    }
    arb::LighterWsSendTxTransport lighter_ws_sendtx;
    lighter_ws_sendtx.start();
    if (lighter_ws_sendtx.wait_until_connected(2000)) {
        lighter_native.set_tx_transport(
            [&lighter_ws_sendtx](std::uint8_t tx_type, const std::string& tx_info_json) {
                return lighter_ws_sendtx.send_tx(tx_type, tx_info_json);
            }
        );
        std::cerr << "[main] lighter order transport=ws_sendtx\n";
    } else {
        std::cerr << "[main] WARNING: lighter ws_sendtx transport unavailable, falling back to HTTP\n";
    }

    // --- WS Exchange Adapters: WS BBO for reads + native signing for writes ---
    arb::WsHyperliquidExchange hl_exchange(feed.hl_bbo_store(), hl_native);
    arb::WsLighterExchange lighter_exchange(feed.lighter_bbo_store(), lighter_native);

    // --- Engine (uses WS exchange adapters) ---
    arb::EngineConfig engine_config;
    engine_config.strategy.spread_bps = spread_bps;
    engine_config.strategy.close_spread_bps = close_spread_bps;
    engine_config.strategy.cancel_band_bps = cancel_band;
    engine_config.strategy.pair_size_usd = pair_size;
    engine_config.strategy.max_position_usd = max_pos;
    engine_config.strategy.max_quote_age_ms = 3000;
    engine_config.strategy.min_rearm_ms = 500;
    engine_config.hl_coin = "HYPE";
    engine_config.lighter_market_id = 24;
    engine_config.dry_run = dry_run;
    engine_config.hl_order_interval_ms = static_cast<std::int64_t>(
        std::stod(env_or("HL_ORDER_INTERVAL_MS", "200")));
    engine_config.lighter_order_interval_ms = static_cast<std::int64_t>(
        std::stod(env_or("LIGHTER_ORDER_INTERVAL_MS", "500")));

    // --- Trade Journal (append-only CSV, flushed with telemetry) ---
    const std::string journal_path = env_or("JOURNAL_PATH", "/home/ubuntu/lighter-hl-arb-cpp/trades.csv");
    arb::TradeJournal journal(journal_path);
    // Write header if file is new
    {
        std::ifstream probe(journal_path);
        if (!probe.good() || probe.peek() == std::ifstream::traits_type::eof()) {
            std::ofstream hdr(journal_path);
            hdr << "timestamp_us,type,hl_side,hl_px,hl_sz,lt_px,lt_sz,spread_bps,hl_oid,lt_tx\n";
        }
    }

    arb::MakerHedgeEngine engine(engine_config, hl_exchange, lighter_exchange, &journal);

    // --- Sync initial HL position from API ---
    {
        const double init_pos = query_hl_position(
            env_or("HL_API_URL", "https://api.hyperliquid.xyz"),
            env_or("HL_USER_ADDRESS", ""),
            engine_config.hl_coin
        );
        if (init_pos != 0.0) {
            engine.set_hl_position(init_pos);
            std::cerr << "[main] synced HL position: " << init_pos << " " << engine_config.hl_coin << "\n";
        }
    }

    // --- Risk ---
    arb::RiskGuard risk({
        .max_exposure_usd = max_pos * 2.0,
        .stale_quote_kill_ms = 5000,
        .max_consecutive_hedge_failures = 3,
        .max_single_trade_usd = pair_size * 2.0,
    });

    // Telemetry.
    std::atomic<uint64_t> tick_count {0};
    std::atomic<uint64_t> trade_count {0};
    auto start_time = std::chrono::steady_clock::now();

    // Wake strategy thread on every BBO update.
    feed.set_on_update([&] { publish_event(); });

    // --- Fill Feed ---
    std::unique_ptr<arb::HlFillFeed> fill_feed;
    if (!hl_user_address.empty()) {
        arb::HlFillFeed::Config fill_cfg;
        fill_cfg.user_address = hl_user_address;
        fill_feed = std::make_unique<arb::HlFillFeed>(fill_cfg);

        fill_feed->set_on_fill([&](const std::string& coin, double price, double size, bool is_buy, const std::string& oid, double fee) {
            if (coin != "HYPE") return;
            // Enqueue fill event — processed on main thread to avoid data races on engine state.
            {
                std::lock_guard lock(g_fill_mu);
                g_fill_queue.push(FillEvent {coin, price, size, is_buy, oid, fee, arb::perf_now_ns()});
            }
            publish_event();
        });

        // BUG FIX 2: Set disconnect callback to immediately activate kill switch
        fill_feed->set_on_disconnect([&](const std::string& reason) {
            if (!risk.kill_switch_active()) {
                risk.activate_kill_switch("fill feed disconnected: " + reason);
                std::cerr << "[risk] " << timestamp_str() << " KILL SWITCH: fill feed disconnect - " << reason << '\n';
                // Cancel any active maker order immediately
                const auto& active = engine.active_hl_oid();
                if (active.has_value()) {
                    hl_exchange.cancel_order("HYPE", *active, dry_run);
                    std::cerr << "[risk] cancelled active HL order due to fill feed disconnect\n";
                }
            }
        });
    }

    // --- Start feeds ---
    std::cerr << "[main] starting market feed...\n";
    feed.start();
    if (fill_feed) {
        std::cerr << "[main] starting fill feed for " << hl_user_address << "\n";
        fill_feed->start();
    }

    // Wait for connectivity.
    std::cerr << "[main] waiting for WS connections...\n";
    for (int i = 0; i < 100 && g_running.load(); ++i) {
        if (feed.hl_connected() && feed.lighter_connected()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!feed.hl_connected() || !feed.lighter_connected()) {
        std::cerr << "[main] WARNING: not all WS connected (HL="
                  << feed.hl_connected() << " Lighter=" << feed.lighter_connected() << ")\n";
    } else {
        std::cerr << "[main] all WS connected, entering strategy loop\n";
    }

    // --- Strategy Loop ---
    uint64_t last_log_tick = 0;
    std::uint64_t last_seen_event_seq = g_event_seq.load(std::memory_order_acquire);
    auto next_housekeeping = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (g_running.load(std::memory_order_relaxed)) {
        auto wake_deadline = next_housekeeping;
        if (const auto retry_ms = engine.next_retry_steady_ms(); retry_ms.has_value()) {
            wake_deadline = std::min(
                wake_deadline,
                std::chrono::steady_clock::time_point {std::chrono::milliseconds {*retry_ms}}
            );
        }
        {
            std::unique_lock lock(g_cv_mu);
            g_cv.wait_until(lock, wake_deadline, [&] {
                return !g_running.load(std::memory_order_relaxed)
                    || g_event_seq.load(std::memory_order_acquire) != last_seen_event_seq;
            });
        }
        if (!g_running.load(std::memory_order_relaxed)) break;
        last_seen_event_seq = g_event_seq.load(std::memory_order_acquire);
        const auto now_steady = std::chrono::steady_clock::now();
        if (now_steady >= next_housekeeping) {
            next_housekeeping = now_steady + std::chrono::seconds(1);
        }

        // --- Process queued fill events on main thread (thread-safe) ---
        {
            std::lock_guard lock(g_fill_mu);
            while (!g_fill_queue.empty()) {
                const auto fill = std::move(g_fill_queue.front());
                g_fill_queue.pop();

                std::cerr << "[fill] " << timestamp_str()
                          << " " << fill.coin << " px=" << fill.price << " sz=" << fill.size
                          << " " << (fill.is_buy ? "BUY" : "SELL") << " oid=" << fill.oid
                          << " fee=" << fill.fee << '\n';

                // BUG FIX 3: Pass the OID to engine so it can handle the race condition
                const auto fill_snap = feed.snapshot();
                const auto logs = engine.on_hl_fill(fill.price, fill.size, fill_snap, fill.oid, fill.local_rx_ns, fill.fee);
                for (const auto& log : logs) {
                    std::cerr << "[engine] " << timestamp_str() << " " << log.message << '\n';
                }
                if (!logs.empty()) {
                    ++trade_count;
                }
            }
        }

        const auto snap = feed.snapshot();
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        ++tick_count;

        arb::PerfCollector::instance().record_hot_path(
            arb::PerfMetric::HlQuoteAgeMs,
            static_cast<std::uint64_t>(std::max<std::int64_t>(snap.hl.quote_age_ms, 0))
        );
        arb::PerfCollector::instance().record_hot_path(
            arb::PerfMetric::LighterQuoteAgeMs,
            static_cast<std::uint64_t>(std::max<std::int64_t>(snap.lighter.quote_age_ms, 0))
        );

        // Skip until both venues have valid data.
        if (snap.hl.bid <= 0.0 || snap.lighter.bid <= 0.0) continue;

        // BUG FIX 2: Check fill feed status every tick (not just in telemetry)
        // Use is_subscribed() instead of is_connected() to ensure we're actually receiving fills
        const bool fills_subscribed = !fill_feed || fill_feed->is_subscribed();
        if (!fills_subscribed && !risk.kill_switch_active()) {
            risk.activate_kill_switch("fill feed not subscribed");
            std::cerr << "[risk] " << timestamp_str() << " KILL SWITCH: fill feed not subscribed\n";
            // Cancel any active maker order immediately
            const auto& active = engine.active_hl_oid();
            if (active.has_value()) {
                hl_exchange.cancel_order("HYPE", *active, dry_run);
                std::cerr << "[risk] cancelled active HL order due to fill feed not subscribed\n";
            }
        } else if (fills_subscribed && risk.kill_switch_active() && 
                   (risk.kill_switch_reason() == "fill feed not subscribed" || 
                    risk.kill_switch_reason().find("fill feed disconnected") != std::string::npos)) {
            // Position reconciliation: check if fills were missed during disconnect
            const double engine_pos = engine.hl_position_base();
            const double actual_pos = query_hl_position(
                env_or("HL_API_URL", "https://api.hyperliquid.xyz"),
                env_or("HL_USER_ADDRESS", ""),
                engine_config.hl_coin
            );
            if (std::abs(actual_pos - engine_pos) > 0.001) {
                std::cerr << "[risk] " << timestamp_str()
                          << " POSITION MISMATCH after fill feed reconnect!"
                          << " engine=" << engine_pos << " actual=" << actual_pos
                          << " diff=" << (actual_pos - engine_pos) << "\n";
                engine.set_hl_position(actual_pos);
                std::cerr << "[risk] " << timestamp_str()
                          << " synced engine position to " << actual_pos << "\n";
            }
            risk.reset_kill_switch();
            std::cerr << "[risk] " << timestamp_str() << " kill switch reset (fill feed reconnected and subscribed)\n";
        }

        // Stale quote kill switch.
        if (snap.lighter.quote_age_ms > 5000 || snap.hl.quote_age_ms > 5000) {
            if (!risk.kill_switch_active()) {
                risk.activate_kill_switch("stale quotes detected");
                std::cerr << "[risk] " << timestamp_str() << " KILL SWITCH: stale quotes"
                          << " hl_age=" << snap.hl.quote_age_ms
                          << " lighter_age=" << snap.lighter.quote_age_ms << '\n';
            }
            continue;
        } else if (risk.kill_switch_active() && risk.kill_switch_reason() == "stale quotes detected") {
            risk.reset_kill_switch();
            std::cerr << "[risk] " << timestamp_str() << " kill switch reset (quotes fresh)\n";
        }
        if (risk.kill_switch_active()) continue;

        // Run engine — collect_snapshot() uses WS exchange adapters (no REST overhead).
        const auto logs = engine.on_market_data(now_ms);
        for (const auto& log : logs) {
            std::cerr << "[engine] " << timestamp_str() << " " << log.message << '\n';
        }

        // Flush journal + telemetry every 500 ticks.
        if (tick_count - last_log_tick >= 500) {
            journal.flush();
            last_log_tick = tick_count.load();
            const auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();

            // BUG FIX 2: Use is_subscribed() instead of is_connected() for more accurate status
            const bool fills_subscribed = !fill_feed || fill_feed->is_subscribed();
            std::cerr << "[telem] " << timestamp_str()
                      << " ticks=" << tick_count << " trades=" << trade_count
                      << " uptime=" << uptime_s << "s"
                      << " version=" << ARB_GIT_VERSION
                      << " spread=" << std::fixed << std::setprecision(2) << snap.cross_spread_bps << "bps"
                      << " hl=" << snap.hl.bid << "/" << snap.hl.ask
                      << " hl_age=" << snap.hl.quote_age_ms << "ms"
                      << " lt=" << snap.lighter.bid << "/" << snap.lighter.ask
                      << " lt_age=" << snap.lighter.quote_age_ms << "ms"
                      << " state=" << static_cast<int>(engine.strategy().state())
                      << " pos=" << std::setprecision(2) << engine.hl_position_base()
                      << " fills_ws=" << (fills_subscribed ? "SUBSCRIBED" : "NOT_SUBSCRIBED") << '\n';

            for (const auto& line : arb::PerfCollector::instance().drain_summary_lines()) {
                std::cerr << "[perf] " << timestamp_str() << ' ' << line << '\n';
            }

            // Note: Kill switch logic for fill feed is now handled every tick above,
            // so we don't need the old lazy check here anymore.
        }
    }

    // --- Shutdown ---
    std::cerr << "\n[main] shutting down...\n";
    journal.flush();
    lighter_position_feed.stop();
    lighter_ws_sendtx.stop();
    hl_ws_post.stop();
    feed.stop();
    if (fill_feed) fill_feed->stop();

    const auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time).count();
    std::cerr << "[main] shutdown complete. ticks=" << tick_count
              << " trades=" << trade_count << " uptime=" << uptime_s << "s\n";
    return 0;
}

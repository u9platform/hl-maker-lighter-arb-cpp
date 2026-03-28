#include "arb/market_feed.hpp"
#include "arb/perf.hpp"

#include <cmath>
#include <iostream>
#include <regex>
#include <string>
#include <thread>

namespace arb {

namespace {

// Fast ad-hoc JSON value extraction — avoids pulling in a JSON library.
// These are hot-path so we use simple string searching instead of regex.

// Parse HL bbo WS message:
// {"channel":"bbo","data":{"coin":"HYPE","time":123,"bbo":[{"px":"21.50","sz":"10","n":3},{"px":"21.51","sz":"5","n":2}]}}
bool parse_hl_bbo(const std::string& msg, double& bid, double& ask) {
    const auto bbo_pos = msg.find("\"bbo\"");
    if (bbo_pos == std::string::npos) return false;

    // First px in bbo array is bid.
    const auto bid_px_pos = msg.find("\"px\"", bbo_pos);
    if (bid_px_pos == std::string::npos) return false;

    auto bid_start = msg.find('"', bid_px_pos + 4);
    if (bid_start == std::string::npos) return false;
    ++bid_start;
    auto bid_end = msg.find('"', bid_start);
    if (bid_end == std::string::npos) return false;
    try { bid = std::stod(msg.substr(bid_start, bid_end - bid_start)); } catch (...) { return false; }

    // Second px is ask.
    const auto ask_px_pos = msg.find("\"px\"", bid_end);
    if (ask_px_pos == std::string::npos) return false;
    auto ask_start = msg.find('"', ask_px_pos + 4);
    if (ask_start == std::string::npos) return false;
    ++ask_start;
    auto ask_end = msg.find('"', ask_start);
    if (ask_end == std::string::npos) return false;
    try { ask = std::stod(msg.substr(ask_start, ask_end - ask_start)); } catch (...) { return false; }

    return bid > 0.0 && ask > 0.0;
}

bool parse_lighter_ticker(const std::string& msg, double& bid, double& ask) {
    const auto ticker_pos = msg.find("\"ticker\"");
    if (ticker_pos == std::string::npos) return false;

    const auto ask_pos = msg.find("\"a\"", ticker_pos);
    const auto bid_pos = msg.find("\"b\"", ticker_pos);
    if (ask_pos == std::string::npos || bid_pos == std::string::npos) return false;

    const auto ask_price_pos = msg.find("\"price\"", ask_pos);
    const auto bid_price_pos = msg.find("\"price\"", bid_pos);
    if (ask_price_pos == std::string::npos || bid_price_pos == std::string::npos) return false;

    auto as = msg.find('"', ask_price_pos + 7);
    auto bs = msg.find('"', bid_price_pos + 7);
    if (as == std::string::npos || bs == std::string::npos) return false;
    ++as;
    ++bs;
    const auto ae = msg.find('"', as);
    const auto be = msg.find('"', bs);
    if (ae == std::string::npos || be == std::string::npos) return false;

    try {
        ask = std::stod(msg.substr(as, ae - as));
        bid = std::stod(msg.substr(bs, be - bs));
    } catch (...) {
        return false;
    }

    return bid > 0.0 && ask > 0.0;
}

std::optional<LighterPositionSnapshot> parse_lighter_position_update(const std::string& msg, int market_index) {
    if (msg.find("\"account_all_positions:\"") == std::string::npos
        || msg.find("\"positions\"") == std::string::npos) {
        return std::nullopt;
    }

    const std::string market_key = "\"" + std::to_string(market_index) + "\":{";
    const auto market_pos = msg.find(market_key);
    if (market_pos == std::string::npos) {
        return LighterPositionSnapshot {};
    }

    const std::string section = msg.substr(market_pos, 512);
    const std::regex sign_pattern(R"REGEX("sign":(-?[0-9]+))REGEX");
    const std::regex pos_pattern(R"REGEX("position":"([^"]+)")REGEX");
    const std::regex avg_pattern(R"REGEX("avg_entry_price":"([^"]+)")REGEX");
    const std::regex val_pattern(R"REGEX("position_value":"([^"]+)")REGEX");

    std::smatch match;
    int sign = 0;
    double position = 0.0;
    LighterPositionSnapshot snap;
    if (std::regex_search(section, match, sign_pattern)) {
        sign = std::stoi(match[1].str());
    }
    if (std::regex_search(section, match, pos_pattern)) {
        position = std::stod(match[1].str());
    }
    snap.size = static_cast<double>(sign) * position;
    if (std::regex_search(section, match, avg_pattern)) {
        snap.avg_entry_price = std::stod(match[1].str());
    }
    if (std::regex_search(section, match, val_pattern)) {
        snap.position_value = std::stod(match[1].str());
    }
    return snap;
}

// Parse HL userFills WS message for fill events.
// {"channel":"userFills","data":{"isSnapshot":false,"user":"0x...","fills":[{"coin":"HYPE","px":"21.50","sz":"2.5","side":"B","oid":12345,...}]}}
struct ParsedFill {
    std::string coin;
    double price {0.0};
    double size {0.0};
    bool is_buy {true};
    std::string oid;
    double fee {0.0};
    std::string fee_token;
};

bool parse_hl_fill(const std::string& msg, std::vector<ParsedFill>& fills) {
    // Only process non-snapshot fills.
    if (msg.find("\"isSnapshot\":true") != std::string::npos) return false;
    if (msg.find("\"fills\"") == std::string::npos) return false;

    // Simple iteration over fill objects.
    std::size_t pos = msg.find("\"fills\"");
    if (pos == std::string::npos) return false;

    // Find each fill object.
    std::size_t search_pos = pos;
    while (true) {
        auto coin_pos = msg.find("\"coin\"", search_pos);
        if (coin_pos == std::string::npos) break;

        ParsedFill fill;

        // Coin.
        auto cs = msg.find('"', coin_pos + 6);
        if (cs == std::string::npos) break;
        ++cs;
        auto ce = msg.find('"', cs);
        if (ce == std::string::npos) break;
        fill.coin = msg.substr(cs, ce - cs);

        // Price.
        auto px_pos = msg.find("\"px\"", ce);
        if (px_pos == std::string::npos) break;
        auto ps = msg.find('"', px_pos + 4);
        if (ps == std::string::npos) break;
        ++ps;
        auto pe = msg.find('"', ps);
        if (pe == std::string::npos) break;
        try { fill.price = std::stod(msg.substr(ps, pe - ps)); } catch (...) { break; }

        // Size.
        auto sz_pos = msg.find("\"sz\"", pe);
        if (sz_pos == std::string::npos) break;
        auto ss = msg.find('"', sz_pos + 4);
        if (ss == std::string::npos) break;
        ++ss;
        auto se = msg.find('"', ss);
        if (se == std::string::npos) break;
        try { fill.size = std::stod(msg.substr(ss, se - ss)); } catch (...) { break; }

        // Side.
        auto side_pos = msg.find("\"side\"", se);
        if (side_pos != std::string::npos) {
            auto side_s = msg.find('"', side_pos + 6);
            if (side_s != std::string::npos) {
                ++side_s;
                fill.is_buy = (msg[side_s] == 'B');
            }
        }

        // OID.
        auto oid_pos = msg.find("\"oid\"", se);
        if (oid_pos != std::string::npos) {
            // oid can be number or string.
            auto oid_colon = msg.find(':', oid_pos + 4);
            if (oid_colon != std::string::npos) {
                auto oid_start = oid_colon + 1;
                while (oid_start < msg.size() && (msg[oid_start] == ' ' || msg[oid_start] == '"')) ++oid_start;
                auto oid_end = oid_start;
                while (oid_end < msg.size() && msg[oid_end] != ',' && msg[oid_end] != '}' && msg[oid_end] != '"') ++oid_end;
                fill.oid = msg.substr(oid_start, oid_end - oid_start);
            }
        }

        // Fee — HL sends "fee":"0.001234" in fill events.
        auto fee_pos = msg.find("\"fee\"", oid_pos != std::string::npos ? oid_pos : se);
        if (fee_pos != std::string::npos && fee_pos < msg.find('}', se)) {
            auto fs = msg.find('"', fee_pos + 5);
            if (fs != std::string::npos) {
                ++fs;
                auto fe = msg.find('"', fs);
                if (fe != std::string::npos) {
                    try { fill.fee = std::stod(msg.substr(fs, fe - fs)); } catch (...) {}
                }
            }
        }

        // Fee token — "feeToken":"USDC"
        auto ft_pos = msg.find("\"feeToken\"", fee_pos != std::string::npos ? fee_pos : se);
        if (ft_pos != std::string::npos && ft_pos < msg.find('}', se) + 100) {
            auto fts = msg.find('"', ft_pos + 10);
            if (fts != std::string::npos) {
                ++fts;
                auto fte = msg.find('"', fts);
                if (fte != std::string::npos) {
                    fill.fee_token = msg.substr(fts, fte - fts);
                }
            }
        }

        fills.push_back(std::move(fill));
        search_pos = se + 1;
    }

    return !fills.empty();
}

// Parse HL trades WS message:
// {"channel":"trades","data":{"coin":"HYPE","time":123,"trades":[{"px":"21.50","sz":"2.5","side":"B","time":123456789,...}]}}
bool parse_hl_trades(const std::string& msg, const std::string& target_coin, std::vector<TradeEvent>& trades) {
    if (msg.find("\"trades\"") == std::string::npos) return false;
    if (msg.find("\"coin\":\"" + target_coin + "\"") == std::string::npos) return false;

    // Find trades array
    std::size_t pos = msg.find("\"trades\"");
    if (pos == std::string::npos) return false;

    // Parse each trade object
    std::size_t search_pos = pos;
    while (true) {
        auto px_pos = msg.find("\"px\"", search_pos);
        if (px_pos == std::string::npos) break;

        TradeEvent trade;
        trade.coin = target_coin;
        trade.timestamp_ns = perf_now_ns(); // Use local timestamp

        // Price
        auto ps = msg.find('"', px_pos + 4);
        if (ps == std::string::npos) break;
        ++ps;
        auto pe = msg.find('"', ps);
        if (pe == std::string::npos) break;
        try { trade.price = std::stod(msg.substr(ps, pe - ps)); } catch (...) { break; }

        // Size
        auto sz_pos = msg.find("\"sz\"", pe);
        if (sz_pos == std::string::npos) break;
        auto ss = msg.find('"', sz_pos + 4);
        if (ss == std::string::npos) break;
        ++ss;
        auto se = msg.find('"', ss);
        if (se == std::string::npos) break;
        try { trade.size = std::stod(msg.substr(ss, se - ss)); } catch (...) { break; }

        // Side
        auto side_pos = msg.find("\"side\"", se);
        if (side_pos != std::string::npos) {
            auto side_s = msg.find('"', side_pos + 6);
            if (side_s != std::string::npos) {
                ++side_s;
                trade.is_buy = (msg[side_s] == 'B');
            }
        }

        // Exchange timestamp (ms) — "time":1774708792573
        auto time_pos = msg.find("\"time\"", side_pos != std::string::npos ? side_pos : se);
        if (time_pos != std::string::npos) {
            auto colon = msg.find(':', time_pos + 5);
            if (colon != std::string::npos) {
                ++colon;
                while (colon < msg.size() && msg[colon] == ' ') ++colon;
                try { trade.exchange_time_ms = std::stoull(msg.substr(colon)); } catch (...) {}
            }
        }

        trades.push_back(std::move(trade));
        search_pos = se + 1;
    }

    return !trades.empty();
}

}  // namespace

// --- MarketFeed ---

MarketFeed::MarketFeed(Config config) : config_(std::move(config)) {}

MarketFeed::~MarketFeed() {
    stop();
}

void MarketFeed::set_on_update(OnBboUpdate cb) {
    on_update_ = std::move(cb);
}

void MarketFeed::set_on_trade(OnTradeCallback cb) {
    on_trade_ = std::move(cb);
}

void MarketFeed::start() {
    // HL WebSocket.
    WsClient::Config hl_cfg;
    hl_cfg.host = config_.hl_ws_host;
    hl_cfg.port = "443";
    hl_cfg.path = config_.hl_ws_path;
    hl_cfg.ping_interval_sec = 20;

    hl_ws_ = std::make_unique<WsClient>(hl_cfg);
    hl_ws_->set_on_message([this](const std::string& msg) { on_hl_message(msg); });
    hl_ws_->set_on_disconnect([this](const std::string& reason) {
        std::cerr << "[hl-ws] disconnected: " << reason << '\n';
        hl_subscribed_.store(false, std::memory_order_release);
        hl_trades_subscribed_.store(false, std::memory_order_release);
        // Re-queue subscribe for reconnect (pending_sends_ was cleared on last handshake).
        subscribe_hl();
    });

    // Queue the subscribe message — it will be sent as soon as WS handshake completes.
    subscribe_hl();
    hl_ws_->connect();

    // Lighter WebSocket.
    WsClient::Config lighter_cfg;
    lighter_cfg.host = config_.lighter_ws_host;
    lighter_cfg.port = "443";
    lighter_cfg.path = config_.lighter_ws_path;
    lighter_cfg.ping_interval_sec = 15;

    lighter_ws_ = std::make_unique<WsClient>(lighter_cfg);
    lighter_ws_->set_on_message([this](const std::string& msg) { on_lighter_message(msg); });
    lighter_ws_->set_on_disconnect([this](const std::string& reason) {
        std::cerr << "[lighter-ws] disconnected: " << reason << '\n';
        lighter_subscribed_.store(false, std::memory_order_release);
        // No need to re-queue subscribe here — Lighter WS sends {"type":"connected"}
        // on reconnect, which triggers subscribe_lighter() in on_lighter_message().
    });
    lighter_ws_->connect();
}

void MarketFeed::stop() {
    if (hl_ws_) hl_ws_->close();
    if (lighter_ws_) lighter_ws_->close();
}

SpreadSnapshot MarketFeed::snapshot() const {
    const Bbo hl = hl_bbo_.load();
    const Bbo lighter = lighter_bbo_.load();
    const double hl_mid = hl.mid();
    const double lighter_mid = lighter.mid();
    const double avg_mid = (hl_mid + lighter_mid) / 2.0;
    const double spread = avg_mid > 0.0 ? ((lighter_mid - hl_mid) / avg_mid) * 10000.0 : 0.0;
    return SpreadSnapshot {
        .lighter = lighter,
        .hl = hl,
        .cross_spread_bps = spread,
    };
}

Bbo MarketFeed::hl_bbo() const { return hl_bbo_.load(); }
Bbo MarketFeed::lighter_bbo() const { return lighter_bbo_.load(); }
bool MarketFeed::hl_connected() const noexcept { return hl_ws_ && hl_ws_->is_connected(); }
bool MarketFeed::lighter_connected() const noexcept { return lighter_ws_ && lighter_ws_->is_connected(); }

void MarketFeed::on_hl_message(const std::string& msg) {
    const std::uint64_t local_rx_ns = perf_now_ns();
    
    // Debug: log first 20 HL WS messages to diagnose missing trades subscription
    {
        static int hl_msg_count = 0;
        if (hl_msg_count < 20) {
            std::cerr << "[hl-ws] msg#" << hl_msg_count << " len=" << msg.size()
                      << " : " << msg.substr(0, 150) << '\n';
            ++hl_msg_count;
        }
    }

    // HL WS protocol: we send subscribe, then get subscriptionResponse, then data.
    if (msg.find("subscriptionResponse") != std::string::npos) {
        std::cerr << "[hl-ws] got subscriptionResponse: " << msg.substr(0, 200) << '\n';
        if (msg.find("\"bbo\"") != std::string::npos && !hl_subscribed_.load(std::memory_order_relaxed)) {
            hl_subscribed_.store(true, std::memory_order_release);
            std::cerr << "[hl-ws] subscribed to bbo:" << config_.hl_coin << '\n';
        }
        if (msg.find("\"trades\"") != std::string::npos && on_trade_ && !hl_trades_subscribed_.load(std::memory_order_relaxed)) {
            hl_trades_subscribed_.store(true, std::memory_order_release);
            std::cerr << "[hl-ws] subscribed to trades:" << config_.hl_coin << '\n';
        }
        // The subscription response may also contain initial snapshot data — fall through to parse.
    }

    // Parse bbo updates.
    double bid = 0.0, ask = 0.0;
    const bool bbo_ok = msg.find("\"bbo\"") != std::string::npos && parse_hl_bbo(msg, bid, ask);
    if (bbo_ok) {
        hl_bbo_.store(bid, ask);
        PerfCollector::instance().record_hot_path(
            PerfMetric::HlMarketLocalRxToBboUpdateNs,
            perf_now_ns() - local_rx_ns
        );
        if (on_update_) on_update_();
    }

    // Parse trade events for speculative hedging
    if (on_trade_ && hl_trades_subscribed_.load(std::memory_order_relaxed)) {
        std::vector<TradeEvent> trades;
        if (parse_hl_trades(msg, config_.hl_coin, trades)) {
            for (const auto& trade : trades) {
                on_trade_(trade);
            }
        }
    }
}

void MarketFeed::on_lighter_message(const std::string& msg) {
    const std::uint64_t local_rx_ns = perf_now_ns();
    // Lighter sends {"type":"connected"} first, then we subscribe.
    if (msg.find("\"connected\"") != std::string::npos && !lighter_subscribed_.load(std::memory_order_relaxed)) {
        subscribe_lighter();
        return;
    }

    // Handle pings from Lighter.
    if (msg.find("\"ping\"") != std::string::npos) {
        lighter_ws_->send("{\"type\":\"pong\"}");
        return;
    }

    if (msg.find("\"ticker\"") != std::string::npos) {
        if (!lighter_subscribed_.load(std::memory_order_relaxed)) {
            lighter_subscribed_.store(true, std::memory_order_release);
            std::cerr << "[lighter-ws] subscribed to ticker:" << config_.lighter_market_id << '\n';
        }

        double bid = 0.0;
        double ask = 0.0;
        if (parse_lighter_ticker(msg, bid, ask)) {
            lighter_bbo_.store(bid, ask);
            PerfCollector::instance().record_hot_path(
                PerfMetric::LighterMarketLocalRxToBboUpdateNs,
                perf_now_ns() - local_rx_ns
            );
            if (on_update_) on_update_();
        }
    }
}

void MarketFeed::subscribe_hl() {
    // Subscribe to BBO for market data
    const std::string sub_bbo = "{\"method\":\"subscribe\",\"subscription\":{\"type\":\"bbo\",\"coin\":\"" +
        config_.hl_coin + "\"}}";
    hl_ws_->send(sub_bbo);

    // Subscribe to trades for speculative hedging if callback is set
    if (on_trade_) {
        const std::string sub_trades = "{\"method\":\"subscribe\",\"subscription\":{\"type\":\"trades\",\"coin\":\"" +
            config_.hl_coin + "\"}}";
        hl_ws_->send(sub_trades);
        std::cerr << "[hl-ws] sent trades subscribe for " << config_.hl_coin << '\n';
    } else {
        std::cerr << "[hl-ws] WARNING: on_trade_ not set, skipping trades subscribe\n";
    }
}

void MarketFeed::subscribe_lighter() {
    const std::string sub = "{\"type\":\"subscribe\",\"channel\":\"ticker/" +
        std::to_string(config_.lighter_market_id) + "\"}";
    lighter_ws_->send(sub);
}

// --- HlFillFeed ---

HlFillFeed::HlFillFeed(Config config) : config_(std::move(config)) {}

HlFillFeed::~HlFillFeed() {
    stop();
}

void HlFillFeed::set_on_fill(FillCallback cb) {
    on_fill_ = std::move(cb);
}

void HlFillFeed::set_on_disconnect(DisconnectCallback cb) {
    on_disconnect_ = std::move(cb);
}

void HlFillFeed::start() {
    WsClient::Config ws_cfg;
    ws_cfg.host = config_.ws_host;
    ws_cfg.port = "443";
    ws_cfg.path = config_.ws_path;
    ws_cfg.ping_interval_sec = 20;

    ws_ = std::make_unique<WsClient>(ws_cfg);
    ws_->set_on_message([this](const std::string& msg) { on_message(msg); });
    ws_->set_on_disconnect([this](const std::string& reason) {
        std::cerr << "[hl-fills] disconnected: " << reason << ", will resubscribe on reconnect\n";
        subscribed_.store(false, std::memory_order_release);
        // BUG FIX 1: Re-queue subscribe for next reconnect.
        // WsClient::send() queues to pending_sends_ when disconnected,
        // which gets flushed on the next successful handshake.
        subscribe();
        // Notify main thread to activate kill switch immediately
        if (on_disconnect_) {
            on_disconnect_(reason);
        }
    });
    
    // BUG FIX 1: Queue the subscribe message before connecting (like MarketFeed does)
    // This ensures that the subscription will be sent as soon as WS handshake completes.
    subscribe();
    ws_->connect();
}

void HlFillFeed::stop() {
    if (ws_) ws_->close();
}

bool HlFillFeed::is_connected() const noexcept {
    return ws_ && ws_->is_connected();
}

bool HlFillFeed::is_subscribed() const noexcept {
    return is_connected() && subscribed_.load(std::memory_order_relaxed);
}

void HlFillFeed::on_message(const std::string& msg) {
    // BUG FIX 1: Check for subscription response to mark as subscribed
    if (!subscribed_.load(std::memory_order_relaxed)) {
        if (msg.find("subscriptionResponse") != std::string::npos) {
            subscribed_.store(true, std::memory_order_release);
            std::cerr << "[hl-fills] subscribed to userFills\n";
            return;
        }
        // Note: We no longer call subscribe() here because it's already queued in start()
        // This prevents the old problem where on_message() might never be called after reconnect
        return;
    }

    // Parse fills.
    std::vector<ParsedFill> fills;
    if (parse_hl_fill(msg, fills) && on_fill_) {
        for (const auto& fill : fills) {
            on_fill_(fill.coin, fill.price, fill.size, fill.is_buy, fill.oid, fill.fee);
        }
    }
}

void HlFillFeed::subscribe() {
    const std::string sub = "{\"method\":\"subscribe\",\"subscription\":{\"type\":\"userFills\",\"user\":\"" +
        config_.user_address + "\"}}";
    ws_->send(sub);
}

LighterPositionFeed::LighterPositionFeed(Config config) : config_(std::move(config)) {}

LighterPositionFeed::~LighterPositionFeed() {
    stop();
}

void LighterPositionFeed::start() {
    WsClient::Config ws_cfg;
    ws_cfg.host = config_.ws_host;
    ws_cfg.port = "443";
    ws_cfg.path = config_.ws_path;
    ws_cfg.ping_interval_sec = 15;

    ws_ = std::make_unique<WsClient>(ws_cfg);
    ws_->set_on_message([this](const std::string& msg) { on_message(msg); });
    ws_->set_on_disconnect([this](const std::string& reason) {
        std::cerr << "[lighter-pos] disconnected: " << reason << '\n';
        subscribed_.store(false, std::memory_order_release);
        subscribe();
        cv_.notify_all();
    });
    subscribe();
    ws_->connect();
}

void LighterPositionFeed::stop() {
    if (ws_) ws_->close();
}

bool LighterPositionFeed::is_connected() const noexcept {
    return ws_ && ws_->is_connected();
}

bool LighterPositionFeed::is_subscribed() const noexcept {
    return is_connected() && subscribed_.load(std::memory_order_relaxed);
}

bool LighterPositionFeed::wait_until_connected(int timeout_ms) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (is_connected()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return is_connected();
}

std::optional<LighterPositionSnapshot> LighterPositionFeed::wait_for_position_change(double baseline_size, int timeout_ms) {
    std::unique_lock lock(mu_);
    const bool ready = cv_.wait_for(
        lock,
        std::chrono::milliseconds(timeout_ms),
        [&] {
            return latest_.has_value()
                && std::abs(latest_->size - baseline_size) > 0.001;
        }
    );
    if (!ready) {
        return std::nullopt;
    }
    return latest_;
}

void LighterPositionFeed::on_message(const std::string& msg) {
    if (msg.find("\"connected\"") != std::string::npos && !subscribed_.load(std::memory_order_relaxed)) {
        subscribe();
        return;
    }

    if (msg.find("\"ping\"") != std::string::npos) {
        ws_->send("{\"type\":\"pong\"}");
        return;
    }

    if (!subscribed_.load(std::memory_order_relaxed)
        && msg.find("\"subscribed/account_all_positions\"") != std::string::npos) {
        subscribed_.store(true, std::memory_order_release);
        std::cerr << "[lighter-pos] subscribed to account_all_positions:" << config_.account_index << '\n';
    }

    const auto snap = parse_lighter_position_update(msg, config_.market_index);
    if (!snap.has_value()) {
        return;
    }

    {
        std::lock_guard lock(mu_);
        latest_ = *snap;
        ++version_;
    }
    cv_.notify_all();
}

void LighterPositionFeed::subscribe() {
    const std::string sub = "{\"type\":\"subscribe\",\"channel\":\"account_all_positions/"
        + std::to_string(config_.account_index)
        + "\",\"auth\":\"" + config_.auth_token + "\"}";
    ws_->send(sub);
}

}  // namespace arb

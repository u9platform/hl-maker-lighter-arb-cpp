#include "arb/market_feed.hpp"

#include <iostream>
#include <regex>
#include <string>

namespace arb {

namespace {

// Fast ad-hoc JSON value extraction — avoids pulling in a JSON library.
// These are hot-path so we use simple string searching instead of regex.

// Parse HL l2Book WS message:
// {"channel":"l2Book","data":{"coin":"HYPE","levels":[[{"px":"21.50","sz":"10",...},...],[{"px":"21.51",...},...]],"time":1234}}
// We only need the top bid and top ask.
bool parse_hl_l2book(const std::string& msg, double& bid, double& ask) {
    // Find "levels":[[{...  — first level array is bids, second is asks.
    const auto levels_pos = msg.find("\"levels\"");
    if (levels_pos == std::string::npos) return false;

    // Find the first "px" after levels — that's top bid.
    const auto bid_px_pos = msg.find("\"px\"", levels_pos);
    if (bid_px_pos == std::string::npos) return false;

    auto bid_start = msg.find('"', bid_px_pos + 4);
    if (bid_start == std::string::npos) return false;
    ++bid_start;
    auto bid_end = msg.find('"', bid_start);
    if (bid_end == std::string::npos) return false;

    try {
        bid = std::stod(msg.substr(bid_start, bid_end - bid_start));
    } catch (...) {
        return false;
    }

    // Find the "],["  separator between bids and asks arrays.
    const auto sep_pos = msg.find("],[", bid_px_pos);
    if (sep_pos == std::string::npos) return false;

    // First "px" after separator is top ask.
    const auto ask_px_pos = msg.find("\"px\"", sep_pos);
    if (ask_px_pos == std::string::npos) return false;

    auto ask_start = msg.find('"', ask_px_pos + 4);
    if (ask_start == std::string::npos) return false;
    ++ask_start;
    auto ask_end = msg.find('"', ask_start);
    if (ask_end == std::string::npos) return false;

    try {
        ask = std::stod(msg.substr(ask_start, ask_end - ask_start));
    } catch (...) {
        return false;
    }

    return bid > 0.0 && ask > 0.0;
}

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

// Parse Lighter orderbook — extract best bid and ask from the message.
// Returns true if at least one side was parsed. bid/ask are set to 0.0 if not found.
// Handles both snapshot and delta messages.
bool parse_lighter_orderbook_sides(const std::string& msg, double& bid, double& ask) {
    bid = 0.0;
    ask = 0.0;
    bool found_any = false;

    // Extract best bid from "bids":[{"price":"X",...},...] — first element.
    const auto bids_pos = msg.find("\"bids\"");
    if (bids_pos != std::string::npos) {
        // Check if bids array is not empty.
        const auto bids_bracket = msg.find('[', bids_pos);
        if (bids_bracket != std::string::npos && bids_bracket + 1 < msg.size() && msg[bids_bracket + 1] != ']') {
            const auto bid_price_pos = msg.find("\"price\"", bids_bracket);
            if (bid_price_pos != std::string::npos) {
                auto bs = msg.find('"', bid_price_pos + 7);
                if (bs != std::string::npos) {
                    ++bs;
                    auto be = msg.find('"', bs);
                    if (be != std::string::npos) {
                        try { bid = std::stod(msg.substr(bs, be - bs)); found_any = true; } catch (...) {}
                    }
                }
            }
        }
    }

    // Extract best ask from "asks":[{"price":"X",...},...].
    const auto asks_pos = msg.find("\"asks\"");
    if (asks_pos != std::string::npos) {
        const auto asks_bracket = msg.find('[', asks_pos);
        if (asks_bracket != std::string::npos && asks_bracket + 1 < msg.size() && msg[asks_bracket + 1] != ']') {
            const auto ask_price_pos = msg.find("\"price\"", asks_bracket);
            if (ask_price_pos != std::string::npos) {
                auto as = msg.find('"', ask_price_pos + 7);
                if (as != std::string::npos) {
                    ++as;
                    auto ae = msg.find('"', as);
                    if (ae != std::string::npos) {
                        try { ask = std::stod(msg.substr(as, ae - as)); found_any = true; } catch (...) {}
                    }
                }
            }
        }
    }

    return found_any;
}

// Parse HL userFills WS message for fill events.
// {"channel":"userFills","data":{"isSnapshot":false,"user":"0x...","fills":[{"coin":"HYPE","px":"21.50","sz":"2.5","side":"B","oid":12345,...}]}}
struct ParsedFill {
    std::string coin;
    double price {0.0};
    double size {0.0};
    bool is_buy {true};
    std::string oid;
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

        fills.push_back(std::move(fill));
        search_pos = se + 1;
    }

    return !fills.empty();
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
    // HL WS protocol: we send subscribe, then get subscriptionResponse, then data.
    if (!hl_subscribed_.load(std::memory_order_relaxed)) {
        if (msg.find("subscriptionResponse") != std::string::npos) {
            hl_subscribed_.store(true, std::memory_order_release);
            std::cerr << "[hl-ws] subscribed to l2Book:" << config_.hl_coin << '\n';
        }
        // The subscription response may also contain initial snapshot data — fall through to parse.
    }

    // Parse l2Book or bbo update.
    double bid = 0.0, ask = 0.0;
    bool ok = false;
    if (msg.find("\"levels\"") != std::string::npos) {
        ok = parse_hl_l2book(msg, bid, ask);
    } else if (msg.find("\"bbo\"") != std::string::npos) {
        ok = parse_hl_bbo(msg, bid, ask);
    }
    if (ok) {
        hl_bbo_.store(bid, ask);
        if (on_update_) on_update_();
    }
}

void MarketFeed::on_lighter_message(const std::string& msg) {
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

    // Parse orderbook.
    if (msg.find("\"order_book\"") != std::string::npos) {
        if (!lighter_subscribed_.load(std::memory_order_relaxed) &&
            msg.find("\"subscribed/order_book\"") != std::string::npos) {
            lighter_subscribed_.store(true, std::memory_order_release);
            std::cerr << "[lighter-ws] subscribed to order_book:" << config_.lighter_market_id << '\n';
        }

        double bid = 0.0, ask = 0.0;
        if (parse_lighter_orderbook_sides(msg, bid, ask)) {
            // For delta updates, only one side may be present.
            // Merge with existing BBO.
            const Bbo current = lighter_bbo_.load();
            if (bid <= 0.0) bid = current.bid;
            if (ask <= 0.0) ask = current.ask;
            if (bid > 0.0 && ask > 0.0) {
                lighter_bbo_.store(bid, ask);
                if (on_update_) on_update_();
            }
        }
    }
}

void MarketFeed::subscribe_hl() {
    // Subscribe to both l2Book (full snapshot every ~0.5s) and bbo (on change, lower latency).
    const std::string sub_book = "{\"method\":\"subscribe\",\"subscription\":{\"type\":\"l2Book\",\"coin\":\"" +
        config_.hl_coin + "\"}}";
    hl_ws_->send(sub_book);
}

void MarketFeed::subscribe_lighter() {
    const std::string sub = "{\"type\":\"subscribe\",\"channel\":\"order_book/" +
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
    });
    ws_->connect();
}

void HlFillFeed::stop() {
    if (ws_) ws_->close();
}

bool HlFillFeed::is_connected() const noexcept {
    return ws_ && ws_->is_connected();
}

void HlFillFeed::on_message(const std::string& msg) {
    if (!subscribed_.load(std::memory_order_relaxed)) {
        if (msg.find("subscriptionResponse") != std::string::npos) {
            subscribed_.store(true, std::memory_order_release);
            std::cerr << "[hl-fills] subscribed to userFills\n";
            return;
        }
        subscribe();
        return;
    }

    // Parse fills.
    std::vector<ParsedFill> fills;
    if (parse_hl_fill(msg, fills) && on_fill_) {
        for (const auto& fill : fills) {
            on_fill_(fill.coin, fill.price, fill.size, fill.is_buy, fill.oid);
        }
    }
}

void HlFillFeed::subscribe() {
    const std::string sub = "{\"method\":\"subscribe\",\"subscription\":{\"type\":\"userFills\",\"user\":\"" +
        config_.user_address + "\"}}";
    ws_->send(sub);
}

}  // namespace arb

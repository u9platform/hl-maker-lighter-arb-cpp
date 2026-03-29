#include "arb/engine.hpp"
#include "arb/native_trading.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct FakeHlExchange final : arb::HyperliquidExchange {
    arb::Bbo bbo {.bid = 10.0, .ask = 10.01, .quote_age_ms = 1};
    arb::HlLimitOrderAck limit_ack {.ok = true, .message = "", .oid = "oid-1"};
    arb::HlIocOrderAck ioc_ack {.ok = true, .message = "", .filled_size = 2.5, .avg_fill_price = 10.02};
    arb::HlCancelAck cancel_ack {.ok = true, .message = "", .oid = "oid-1"};
    arb::HlReduceAck reduce_ack {.ok = true, .message = "", .filled_size = 2.5, .avg_fill_price = 10.0};
    int place_count {0};
    int ioc_count {0};
    int cancel_count {0};
    int reduce_count {0};
    arb::HlLimitOrderRequest last_limit {};
    arb::HlIocOrderRequest last_ioc {};

    arb::Bbo get_bbo(const std::string&) override { return bbo; }
    arb::HlLimitOrderAck place_limit_order(const arb::HlLimitOrderRequest& request) override {
        ++place_count;
        last_limit = request;
        return limit_ack;
    }
    arb::HlIocOrderAck place_ioc_order(const arb::HlIocOrderRequest& request) override {
        ++ioc_count;
        last_ioc = request;
        return ioc_ack;
    }
    arb::HlCancelAck cancel_order(const std::string&, const std::string&, bool) override {
        ++cancel_count;
        return cancel_ack;
    }
    arb::HlReduceAck reduce_position(const std::string&, bool, double, bool) override {
        ++reduce_count;
        return reduce_ack;
    }
};

struct FakeLighterExchange final : arb::LighterExchange {
    arb::Bbo bbo {.bid = 10.03, .ask = 10.05, .quote_age_ms = 1};
    arb::LighterLimitOrderAck limit_ack {.ok = true, .resting_confirmed = true, .message = "", .tx_hash = "ltx-1", .client_order_index = 1001, .order_index = 2001};
    arb::LighterCancelAck cancel_ack {.ok = true, .message = "", .tx_hash = "ctx-1", .order_index = 1001};
    arb::LighterIocAck ioc_ack {.ok = true, .message = "", .tx_hash = "tx-1"};
    int limit_count {0};
    int cancel_count {0};
    int ioc_count {0};
    arb::LighterLimitOrderRequest last_limit {};
    arb::LighterIocRequest last_ioc {};

    arb::Bbo get_bbo(std::int64_t) override { return bbo; }
    arb::LighterLimitOrderAck place_limit_order(const arb::LighterLimitOrderRequest& request) override {
        ++limit_count;
        last_limit = request;
        return limit_ack;
    }
    arb::LighterCancelAck cancel_order(std::int64_t, bool) override {
        ++cancel_count;
        return cancel_ack;
    }
    arb::LighterIocAck place_ioc_order(const arb::LighterIocRequest& request) override {
        ++ioc_count;
        last_ioc = request;
        return ioc_ack;
    }
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_places_maker_when_spread_reaches_threshold() {
    FakeHlExchange hl;
    FakeLighterExchange lighter;
    arb::EngineConfig config;
    config.strategy.spread_bps = 2.0;
    config.strategy.cancel_band_bps = 0.5;
    config.hl_order_interval_ms = 0;
    config.spec_hedge_min_cross_bps = 0.0;
    config.spec_hedge_min_trade_ratio = 0.0;
    arb::MakerHedgeEngine engine(config, hl, lighter);

    const auto logs = engine.on_market_data(1000);
    require(hl.place_count == 1, "expected one HL maker placement");
    require(!logs.empty(), "expected a placement log");
    require(engine.active_hl_oid().has_value(), "expected an active HL oid");
}

void test_cancels_when_spread_reverts_below_band() {
    FakeHlExchange hl;
    FakeLighterExchange lighter;
    arb::EngineConfig config;
    config.strategy.spread_bps = 2.0;
    config.strategy.cancel_band_bps = 0.5;
    config.hl_order_interval_ms = 0;
    arb::MakerHedgeEngine engine(config, hl, lighter);

    const auto initial_logs = engine.on_market_data(1000);
    require(!initial_logs.empty(), "expected initial placement logs");
    lighter.bbo = {.bid = 10.003, .ask = 10.007, .quote_age_ms = 1};
    hl.bbo = {.bid = 10.0, .ask = 10.01, .quote_age_ms = 1};

    const auto logs = engine.on_market_data(2000);
    require(hl.cancel_count == 1, "expected one HL cancel");
    require(!logs.empty(), "expected a cancel log");
    require(!engine.active_hl_oid().has_value(), "expected active oid cleared after cancel");
}

void test_cancel_not_blocked_by_recent_place_rate_limit() {
    FakeHlExchange hl;
    FakeLighterExchange lighter;
    arb::EngineConfig config;
    config.strategy.spread_bps = 2.0;
    config.strategy.cancel_band_bps = 0.5;
    config.hl_order_interval_ms = 200;
    arb::MakerHedgeEngine engine(config, hl, lighter);

    const auto initial_logs = engine.on_market_data(1000);
    require(!initial_logs.empty(), "expected initial placement logs");
    require(hl.place_count == 1, "expected one HL maker placement");

    lighter.bbo = {.bid = 10.003, .ask = 10.007, .quote_age_ms = 1};
    hl.bbo = {.bid = 10.0, .ask = 10.01, .quote_age_ms = 1};

    const auto logs = engine.on_market_data(1100);
    require(hl.cancel_count == 1, "expected cancel to bypass recent place limiter");
    require(!logs.empty(), "expected cancel log");
}

void test_cancel_failed_does_not_rearm_second_maker() {
    FakeHlExchange hl;
    FakeLighterExchange lighter;
    arb::EngineConfig config;
    config.strategy.spread_bps = 2.0;
    config.strategy.cancel_band_bps = 0.5;
    config.hl_order_interval_ms = 0;
    arb::MakerHedgeEngine engine(config, hl, lighter);

    auto initial_logs = engine.on_market_data(1000);
    require(!initial_logs.empty(), "expected initial placement");
    require(hl.place_count == 1, "expected first maker placement");

    hl.cancel_ack.ok = false;
    hl.cancel_ack.message = "cancel failed";
    lighter.bbo = {.bid = 10.003, .ask = 10.007, .quote_age_ms = 1};
    hl.bbo = {.bid = 10.0, .ask = 10.01, .quote_age_ms = 1};
    auto cancel_logs = engine.on_market_data(2000);
    require(!cancel_logs.empty(), "expected cancel attempt");
    require(hl.cancel_count == 1, "expected one cancel attempt");
    require(engine.active_hl_oid().has_value(), "expected original oid to stay active after failed cancel");

    lighter.bbo = {.bid = 10.03, .ask = 10.05, .quote_age_ms = 1};
    hl.bbo = {.bid = 10.0, .ask = 10.01, .quote_age_ms = 1};
    auto rearm_logs = engine.on_market_data(3000);
    require(rearm_logs.empty(), "expected no new maker placement while cancel pending");
    require(hl.place_count == 1, "expected no second maker placement");
}

void test_sends_lighter_hedge_after_hl_fill() {
    FakeHlExchange hl;
    FakeLighterExchange lighter;
    lighter.ioc_ack.fill_confirmed = true;
    lighter.ioc_ack.confirmed_size = 2.5;
    arb::EngineConfig config;
    config.hl_order_interval_ms = 0;
    arb::MakerHedgeEngine engine(config, hl, lighter);

    const auto initial_logs = engine.on_market_data(1000);
    require(!initial_logs.empty(), "expected initial placement logs");
    const arb::SpreadSnapshot snapshot = engine.collect_snapshot();
    const auto logs = engine.on_hl_fill(10.0, 2.5, snapshot, "oid-1", 1000);

    require(lighter.ioc_count == 1, "expected one lighter IOC");
    require(!logs.empty(), "expected hedge log");
    require(engine.strategy().state() == arb::StrategyState::Idle,
            "expected strategy to reset after confirmed hedge");
}

void test_hedge_reject_triggers_hl_unwind() {
    FakeHlExchange hl;
    FakeLighterExchange lighter;
    lighter.ioc_ack.ok = false;
    lighter.ioc_ack.message = "reject";
    arb::EngineConfig config;
    config.hl_order_interval_ms = 0;
    arb::MakerHedgeEngine engine(config, hl, lighter);

    const auto initial_logs = engine.on_market_data(1000);
    require(!initial_logs.empty(), "expected initial placement logs");
    const arb::SpreadSnapshot snapshot = engine.collect_snapshot();
    const auto logs = engine.on_hl_fill(10.0, 2.5, snapshot, "oid-1", 1000);
    require(hl.reduce_count == 1, "expected one HL unwind");
    require(!logs.empty(), "expected unwind log");
}

void test_maker_price_uses_lighter_anchor_with_hl_post_only_clamp() {
    FakeHlExchange hl;
    FakeLighterExchange lighter;
    arb::EngineConfig config;
    config.strategy.spread_bps = 2.0;
    config.strategy.cancel_band_bps = 0.5;
    config.strategy.pair_size_usd = 25.0;
    config.hl_order_interval_ms = 0;
    arb::MakerHedgeEngine engine(config, hl, lighter);

    const auto logs = engine.on_market_data(1000);
    require(!logs.empty(), "expected placement logs");
    require(hl.place_count == 1, "expected one HL maker placement");
    require(hl.last_limit.is_buy, "expected HL buy maker for positive spread");
    require(hl.last_limit.price == hl.bbo.bid, "expected HL price clamped to best bid for post-only safety");
    require(hl.last_limit.size > 0.0, "expected positive order size");
}

void test_hl_ioc_request_plumbing() {
    FakeHlExchange hl;
    FakeLighterExchange lighter;
    const auto ack = hl.place_ioc_order(arb::HlIocOrderRequest {
        .coin = "HYPE",
        .is_buy = false,
        .price = 9.95,
        .size = 1.25,
        .dry_run = false,
    });
    require(hl.ioc_count == 1, "expected one HL IOC");
    require(!hl.last_ioc.is_buy, "expected sell taker IOC");
    require(ack.ok, "expected fake HL IOC ack");
}

void test_lighter_limit_and_cancel_plumbing() {
    FakeHlExchange hl;
    FakeLighterExchange lighter;
    const auto limit_ack = lighter.place_limit_order(arb::LighterLimitOrderRequest {
        .is_ask = true,
        .price = 10.10,
        .size = 1.5,
        .post_only = true,
        .dry_run = false,
    });
    require(lighter.limit_count == 1, "expected one lighter limit placement");
    require(lighter.last_limit.is_ask, "expected lighter maker ask");
    require(limit_ack.ok, "expected fake lighter maker ack");

    const auto cancel_ack = lighter.cancel_order(limit_ack.order_index, false);
    require(lighter.cancel_count == 1, "expected one lighter cancel");
    require(cancel_ack.ok, "expected fake lighter cancel ack");
}

void test_native_lighter_cancel_requires_confirmed_order_index() {
    arb::NativeLighterTrading lighter(arb::LighterConfig {});
    const auto ack = lighter.cancel_order(0, false);
    require(!ack.ok, "expected lighter cancel to reject missing order_index");
}

void test_speculative_reconciliation_closes_order_lifecycle() {
    FakeHlExchange hl;
    FakeLighterExchange lighter;
    lighter.ioc_ack.fill_confirmed = true;
    lighter.ioc_ack.confirmed_size = 2.5;
    lighter.ioc_ack.fill_price = 10.04;

    arb::EngineConfig config;
    config.strategy.spread_bps = 2.0;
    config.strategy.cancel_band_bps = 0.5;
    config.hl_order_interval_ms = 0;
    arb::MakerHedgeEngine engine(config, hl, lighter);

    auto initial_logs = engine.on_market_data(1000);
    require(!initial_logs.empty(), "expected placement");
    const auto snapshot = engine.collect_snapshot();

    arb::TradeEvent trade;
    trade.coin = "HYPE";
    trade.price = hl.last_limit.price - 0.02;
    trade.size = hl.last_limit.size;
    trade.is_buy = false;
    trade.timestamp_ns = 123456789;
    trade.exchange_time_ms = 9999999999999;
    auto spec_logs = engine.on_trade_event(trade);
    require(!spec_logs.empty(), "expected speculative hedge");

    auto fill_logs = engine.on_hl_fill(hl.last_limit.price, hl.last_limit.size, snapshot, "oid-1", 1001, 0.0);
    require(!fill_logs.empty(), "expected reconciliation logs");
    require(!engine.active_hl_oid().has_value(), "expected active oid cleared after speculative full reconciliation");
    require(engine.strategy().state() == arb::StrategyState::Idle, "expected strategy reset after speculative full reconciliation");
}

}  // namespace

int main() {
    const std::vector<void (*)()> tests {
        test_places_maker_when_spread_reaches_threshold,
        test_cancels_when_spread_reverts_below_band,
        test_cancel_not_blocked_by_recent_place_rate_limit,
        test_cancel_failed_does_not_rearm_second_maker,
        test_sends_lighter_hedge_after_hl_fill,
        test_hedge_reject_triggers_hl_unwind,
        test_maker_price_uses_lighter_anchor_with_hl_post_only_clamp,
        test_hl_ioc_request_plumbing,
        test_lighter_limit_and_cancel_plumbing,
        test_native_lighter_cancel_requires_confirmed_order_index,
        test_speculative_reconciliation_closes_order_lifecycle,
    };

    for (const auto& test : tests) {
        test();
    }

    std::cout << "All tests passed\n";
    return EXIT_SUCCESS;
}

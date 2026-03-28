#include "arb/engine.hpp"

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
    arb::HlCancelAck cancel_ack {.ok = true, .message = "", .oid = "oid-1"};
    arb::HlReduceAck reduce_ack {.ok = true, .message = "", .filled_size = 2.5, .avg_fill_price = 10.0};
    int place_count {0};
    int cancel_count {0};
    int reduce_count {0};
    arb::HlLimitOrderRequest last_limit {};

    arb::Bbo get_bbo(const std::string&) override { return bbo; }
    arb::HlLimitOrderAck place_limit_order(const arb::HlLimitOrderRequest& request) override {
        ++place_count;
        last_limit = request;
        return limit_ack;
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
    arb::LighterIocAck ioc_ack {.ok = true, .message = "", .tx_hash = "tx-1"};
    int ioc_count {0};
    arb::LighterIocRequest last_ioc {};

    arb::Bbo get_bbo(std::int64_t) override { return bbo; }
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

void test_sends_lighter_hedge_after_hl_fill() {
    FakeHlExchange hl;
    FakeLighterExchange lighter;
    lighter.ioc_ack.fill_confirmed = true;
    lighter.ioc_ack.confirmed_size = 2.5;
    arb::EngineConfig config;
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
    arb::MakerHedgeEngine engine(config, hl, lighter);

    const auto initial_logs = engine.on_market_data(1000);
    require(!initial_logs.empty(), "expected initial placement logs");
    const arb::SpreadSnapshot snapshot = engine.collect_snapshot();
    const auto logs = engine.on_hl_fill(10.0, 2.5, snapshot, "oid-1", 1000);
    require(hl.reduce_count == 1, "expected one HL unwind");
    require(!logs.empty(), "expected unwind log");
}

}  // namespace

int main() {
    const std::vector<void (*)()> tests {
        test_places_maker_when_spread_reaches_threshold,
        test_cancels_when_spread_reverts_below_band,
        test_sends_lighter_hedge_after_hl_fill,
        test_hedge_reject_triggers_hl_unwind,
    };

    for (const auto& test : tests) {
        test();
    }

    std::cout << "All tests passed\n";
    return EXIT_SUCCESS;
}

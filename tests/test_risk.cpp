#include "arb/risk.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

namespace {

arb::SpreadSnapshot make_snap(int64_t hl_age = 100, int64_t lt_age = 100) {
    arb::SpreadSnapshot snap;
    snap.hl.bid = 38.0;
    snap.hl.ask = 38.01;
    snap.hl.quote_age_ms = hl_age;
    snap.lighter.bid = 38.05;
    snap.lighter.ask = 38.06;
    snap.lighter.quote_age_ms = lt_age;
    snap.cross_spread_bps = 1.3;
    return snap;
}

void test_ok() {
    arb::RiskGuard risk(arb::RiskConfig {
        .max_exposure_usd = 200.0,
        .stale_quote_kill_ms = 5000,
        .max_consecutive_hedge_failures = 3,
        .max_single_trade_usd = 50.0,
    });

    auto snap = make_snap();
    assert(risk.check_new_entry(25.0, snap) == arb::RiskVeto::Ok);
    std::cerr << "  ok: basic entry allowed\n";
}

void test_max_exposure() {
    arb::RiskGuard risk(arb::RiskConfig {.max_exposure_usd = 100.0});
    risk.set_exposure(90.0);
    auto snap = make_snap();
    assert(risk.check_new_entry(20.0, snap) == arb::RiskVeto::MaxExposure);
    assert(risk.check_new_entry(5.0, snap) == arb::RiskVeto::Ok);
    std::cerr << "  ok: max exposure check\n";
}

void test_stale_quotes() {
    arb::RiskGuard risk(arb::RiskConfig {.stale_quote_kill_ms = 5000});
    auto snap = make_snap(6000, 100);
    assert(risk.check_new_entry(25.0, snap) == arb::RiskVeto::StaleQuotes);
    snap = make_snap(100, 6000);
    assert(risk.check_new_entry(25.0, snap) == arb::RiskVeto::StaleQuotes);
    snap = make_snap(100, 100);
    assert(risk.check_new_entry(25.0, snap) == arb::RiskVeto::Ok);
    std::cerr << "  ok: stale quotes check\n";
}

void test_hedge_failures() {
    arb::RiskGuard risk(arb::RiskConfig {.max_consecutive_hedge_failures = 3});
    auto snap = make_snap();
    risk.on_hedge_failure();
    risk.on_hedge_failure();
    assert(risk.check_new_entry(25.0, snap) == arb::RiskVeto::Ok);
    risk.on_hedge_failure();
    assert(risk.check_new_entry(25.0, snap) == arb::RiskVeto::KillSwitchActive);
    assert(risk.kill_switch_active());
    risk.reset_kill_switch();
    assert(!risk.kill_switch_active());
    assert(risk.check_new_entry(25.0, snap) == arb::RiskVeto::Ok);
    std::cerr << "  ok: hedge failure tracking\n";
}

void test_hedge_success_resets() {
    arb::RiskGuard risk(arb::RiskConfig {.max_consecutive_hedge_failures = 3});
    auto snap = make_snap();
    risk.on_hedge_failure();
    risk.on_hedge_failure();
    risk.on_hedge_success();
    assert(risk.consecutive_hedge_failures() == 0);
    risk.on_hedge_failure();
    assert(risk.check_new_entry(25.0, snap) == arb::RiskVeto::Ok);
    std::cerr << "  ok: hedge success resets counter\n";
}

void test_trade_too_large() {
    arb::RiskGuard risk(arb::RiskConfig {.max_single_trade_usd = 50.0});
    auto snap = make_snap();
    assert(risk.check_new_entry(51.0, snap) == arb::RiskVeto::TradeTooLarge);
    assert(risk.check_new_entry(49.0, snap) == arb::RiskVeto::Ok);
    std::cerr << "  ok: trade too large check\n";
}

void test_kill_switch() {
    arb::RiskGuard risk(arb::RiskConfig {});
    auto snap = make_snap();
    risk.activate_kill_switch("manual");
    assert(risk.check_new_entry(25.0, snap) == arb::RiskVeto::KillSwitchActive);
    assert(risk.kill_switch_reason() == "manual");
    risk.reset_kill_switch();
    assert(risk.check_new_entry(25.0, snap) == arb::RiskVeto::Ok);
    std::cerr << "  ok: manual kill switch\n";
}

}  // namespace

int main() {
    std::cerr << "=== Risk Guard Tests ===\n";
    test_ok();
    test_max_exposure();
    test_stale_quotes();
    test_hedge_failures();
    test_hedge_success_resets();
    test_trade_too_large();
    test_kill_switch();
    std::cerr << "All risk tests passed\n";
    return 0;
}

#include "arb/market_feed.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <thread>

namespace {
std::atomic<bool> g_running {true};
void signal_handler(int) { g_running = false; }
}

int main() {
    std::signal(SIGINT, signal_handler);

    std::cerr << "=== WS Market Feed Smoke Test ===\n";
    std::cerr << "Connecting to HL + Lighter WebSockets...\n";

    arb::MarketFeed::Config cfg;
    cfg.hl_coin = "HYPE";
    cfg.lighter_market_id = 24;

    arb::MarketFeed feed(cfg);

    std::atomic<int> update_count {0};
    feed.set_on_update([&] { ++update_count; });

    feed.start();

    // Wait up to 15s for both to connect.
    for (int i = 0; i < 150 && g_running; ++i) {
        if (feed.hl_connected() && feed.lighter_connected()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cerr << "HL connected: " << feed.hl_connected()
              << " | Lighter connected: " << feed.lighter_connected() << '\n';

    if (!feed.hl_connected() || !feed.lighter_connected()) {
        std::cerr << "WARN: not all connections established, will print what we get\n";
    }

    // Print BBO for 30 seconds.
    const auto start = std::chrono::steady_clock::now();
    int last_count = 0;
    while (g_running) {
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(30)) break;

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        const auto snap = feed.snapshot();
        const int cur_count = update_count.load();

        std::cerr << std::fixed << std::setprecision(4)
                  << "HL bid=" << snap.hl.bid << " ask=" << snap.hl.ask
                  << " age=" << snap.hl.quote_age_ms << "ms"
                  << " | LT bid=" << snap.lighter.bid << " ask=" << snap.lighter.ask
                  << " age=" << snap.lighter.quote_age_ms << "ms"
                  << " | spread=" << std::setprecision(2) << snap.cross_spread_bps << "bps"
                  << " | updates=" << cur_count << " (+" << (cur_count - last_count) << "/s)\n";
        last_count = cur_count;
    }

    std::cerr << "\nTotal updates received: " << update_count << '\n';
    feed.stop();
    std::cerr << "Shutdown OK\n";
    return 0;
}

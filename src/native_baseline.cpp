#include "arb/public_exchange.hpp"

#include <chrono>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {

struct SampleStats {
    double mean_ms {0.0};
    double max_ms {0.0};
};

template <typename Fn>
SampleStats measure(int samples, Fn&& fn) {
    std::vector<double> values;
    values.reserve(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        const auto end = std::chrono::steady_clock::now();
        values.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    const double sum = std::accumulate(values.begin(), values.end(), 0.0);
    const double max_v = *std::max_element(values.begin(), values.end());
    return SampleStats {.mean_ms = sum / static_cast<double>(values.size()), .max_ms = max_v};
}

}  // namespace

int main(int argc, char** argv) {
    int samples = 5;
    if (argc > 1) {
        samples = std::stoi(argv[1]);
    }

    arb::NativeHyperliquidExchange hl;
    arb::NativeLighterExchange lighter;

    const SampleStats hl_stats = measure(samples, [&] { (void)hl.get_bbo("HYPE"); });
    const SampleStats lighter_stats = measure(samples, [&] { (void)lighter.get_bbo(24); });

    std::cout << "native_hl_orderbook mean_ms=" << hl_stats.mean_ms << " max_ms=" << hl_stats.max_ms << '\n';
    std::cout << "native_lighter_orderbook mean_ms=" << lighter_stats.mean_ms << " max_ms=" << lighter_stats.max_ms << '\n';
    return 0;
}

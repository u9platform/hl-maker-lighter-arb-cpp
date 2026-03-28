#include "arb/perf.hpp"

#include <algorithm>
#include <sstream>

namespace arb {

namespace {

constexpr std::uint64_t kNsPerMs = 1000ULL * 1000ULL;

}  // namespace

PerfCollector& PerfCollector::instance() {
    static PerfCollector collector;
    return collector;
}

PerfCollector::PerfCollector() = default;

void PerfCollector::record_hot_path(PerfMetric metric, std::uint64_t value) noexcept {
    thread_local std::uint32_t seq = 0;
    ++seq;
    if ((seq % hot_path_sample_rate_) != 0U) {
        return;
    }
    record_sample(metric, value);
}

void PerfCollector::record_trade_path(PerfMetric metric, std::uint64_t value) noexcept {
    record_sample(metric, value);
}

void PerfCollector::record_sample(PerfMetric metric, std::uint64_t value) noexcept {
    auto& counter = counters_[static_cast<std::size_t>(metric)];
    counter.count.fetch_add(1, std::memory_order_relaxed);
    counter.sum.fetch_add(value, std::memory_order_relaxed);

    std::uint64_t current_max = counter.max.load(std::memory_order_relaxed);
    while (value > current_max &&
           !counter.max.compare_exchange_weak(current_max, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

std::vector<std::string> PerfCollector::drain_summary_lines() {
    std::vector<std::string> lines;
    for (std::size_t idx = 0; idx < static_cast<std::size_t>(PerfMetric::Count); ++idx) {
        auto& counter = counters_[idx];
        const std::uint64_t count = counter.count.exchange(0, std::memory_order_relaxed);
        const std::uint64_t sum = counter.sum.exchange(0, std::memory_order_relaxed);
        const std::uint64_t max = counter.max.exchange(0, std::memory_order_relaxed);
        if (count == 0) {
            continue;
        }

        const auto metric = static_cast<PerfMetric>(idx);
        std::ostringstream oss;
        oss << "metric=" << metric_name(metric)
            << " samples=" << count
            << " mean=";

        if (unit_name(metric) == "ms") {
            oss.setf(std::ios::fixed);
            oss.precision(3);
            oss << (static_cast<double>(sum) / static_cast<double>(count));
            oss << " max=" << static_cast<double>(max);
        } else {
            oss.setf(std::ios::fixed);
            oss.precision(3);
            oss << (static_cast<double>(sum) / static_cast<double>(count)) / static_cast<double>(kNsPerMs);
            oss << " max=" << static_cast<double>(max) / static_cast<double>(kNsPerMs);
        }

        oss << " unit=" << unit_name(metric);
        if (metric == PerfMetric::HlMarketLocalRxToBboUpdateNs ||
            metric == PerfMetric::LighterMarketLocalRxToBboUpdateNs ||
            metric == PerfMetric::CrossVenueAlignmentMs ||
            metric == PerfMetric::StrategyDecisionNs) {
            oss << " sampled=1";
        }
        lines.push_back(oss.str());
    }

    return lines;
}

std::string PerfCollector::metric_name(PerfMetric metric) {
    switch (metric) {
        case PerfMetric::HlMarketLocalRxToBboUpdateNs: return "hl_market_local_rx_to_bbo_update";
        case PerfMetric::LighterMarketLocalRxToBboUpdateNs: return "lighter_market_local_rx_to_bbo_update";
        case PerfMetric::CrossVenueAlignmentMs: return "cross_venue_alignment";
        case PerfMetric::StrategyDecisionNs: return "strategy_decision";
        case PerfMetric::SignalToHlMakerSendNs: return "signal_to_hl_maker_send";
        case PerfMetric::HlMakerSendToAckNs: return "hl_maker_send_to_ack";
        case PerfMetric::HlMakerRestingLifetimeNs: return "hl_maker_resting_lifetime";
        case PerfMetric::CancelTriggerToHlCancelSendNs: return "cancel_trigger_to_hl_cancel_send";
        case PerfMetric::HlCancelSendToAckNs: return "hl_cancel_send_to_ack";
        case PerfMetric::HlFillLocalRxToLighterSendNs: return "hl_fill_local_rx_to_lighter_send";
        case PerfMetric::LighterSendToAckNs: return "lighter_send_to_ack";
        case PerfMetric::MakerFillToTakerAckTotalNs: return "maker_fill_to_taker_ack_total";
        case PerfMetric::HedgeFailureToUnwindSendNs: return "hedge_failure_to_unwind_send";
        case PerfMetric::UnwindSendToAckNs: return "unwind_send_to_ack";
        case PerfMetric::Count: break;
    }
    return "unknown";
}

std::string PerfCollector::unit_name(PerfMetric metric) {
    if (metric == PerfMetric::CrossVenueAlignmentMs) {
        return "ms";
    }
    return "ms";
}

}  // namespace arb

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace arb {

enum class PerfMetric : std::uint8_t {
    HlMarketLocalRxToBboUpdateNs = 0,
    LighterMarketLocalRxToBboUpdateNs,
    CrossVenueAlignmentMs,
    StrategyDecisionNs,
    SignalToHlMakerSendNs,
    HlMakerSendToAckNs,
    HlMakerRestingLifetimeNs,
    CancelTriggerToHlCancelSendNs,
    HlCancelSendToAckNs,
    HlFillLocalRxToLighterSendNs,
    LighterSendToAckNs,
    MakerFillToTakerAckTotalNs,
    HedgeFailureToUnwindSendNs,
    UnwindSendToAckNs,
    Count,
};

class PerfCollector {
  public:
    static PerfCollector& instance();

    void record_hot_path(PerfMetric metric, std::uint64_t value) noexcept;
    void record_trade_path(PerfMetric metric, std::uint64_t value) noexcept;

    [[nodiscard]] std::vector<std::string> drain_summary_lines();

  private:
    struct Counter {
        std::atomic<std::uint64_t> count {0};
        std::atomic<std::uint64_t> sum {0};
        std::atomic<std::uint64_t> max {0};
    };

    PerfCollector();

    void record_sample(PerfMetric metric, std::uint64_t value) noexcept;
    [[nodiscard]] static std::string metric_name(PerfMetric metric);
    [[nodiscard]] static std::string unit_name(PerfMetric metric);

    std::uint32_t hot_path_sample_rate_ {64};
    Counter counters_[static_cast<std::size_t>(PerfMetric::Count)];
};

[[nodiscard]] inline std::uint64_t perf_now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

}  // namespace arb

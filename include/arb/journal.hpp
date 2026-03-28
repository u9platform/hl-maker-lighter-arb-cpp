#pragma once

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

namespace arb {

struct JournalEntry {
    // Identity
    std::int64_t trade_id {0};       // Unique trade ID (epoch_us based)
    std::int64_t timestamp_us;       // Final completion timestamp
    char type;                       // 'T'=trade, 'U'=unwind, 'F'=hedge_fail
    std::string hedge_status;        // "filled", "unconfirmed_then_unwind", "failed"

    // Timing
    std::int64_t hl_fill_timestamp_us {0};
    std::int64_t lighter_fill_timestamp_us {0};

    // HL fill
    char hl_side;                    // 'B'=buy, 'S'=sell
    double hl_px;                    // Actual HL fill price
    double hl_sz;                    // Actual HL fill size
    double hl_fee {0.0};            // HL fee from fill WS

    // Lighter hedge fill
    double lt_fill_px {0.0};        // Actual Lighter fill price (from position/value delta)
    double lt_sz;                    // Lighter confirmed size
    double lt_fee {0.0};            // Lighter fee

    // Strategy context
    double spread_bps;
    double signal_spread_bps {0.0};
    double maker_resting_ms {0.0};
    double hedge_total_ms {0.0};
    std::string hl_oid;
    std::string lt_tx;

    // Unwind path (only for type='U' or 'F')
    double unwind_fill_px {0.0};
    double unwind_fill_sz {0.0};
    double unwind_fee {0.0};
    std::string failure_reason;
};

// Append-only trade journal.  Hot path (record()) only pushes to an
// in-memory vector.  flush() writes buffered entries to disk and is
// called from the telemetry path (every ~500 ticks) so zero I/O
// touches the latency-critical fill→hedge path.
class TradeJournal {
  public:
    explicit TradeJournal(const std::string& path)
        : path_(path) {
        // Write header if file is new/empty
        std::ifstream check(path);
        if (!check.good() || check.peek() == std::ifstream::traits_type::eof()) {
            std::ofstream out(path);
            out << "trade_id,timestamp_us,type,hedge_status,"
                << "hl_fill_timestamp_us,lighter_fill_timestamp_us,"
                << "hl_side,hl_fill_price,hl_fill_size,hl_fee,"
                << "lighter_fill_price,lighter_fill_size,lighter_fee,"
                << "spread_bps,signal_spread_bps,maker_resting_ms,hedge_total_ms,"
                << "hl_oid,lt_tx,"
                << "unwind_fill_price,unwind_fill_size,unwind_fee,failure_reason\n";
            out.flush();
        }
    }

    // Hot path — no I/O, no allocation (vector reserve'd).
    void record(JournalEntry entry) {
        buf_.push_back(std::move(entry));
    }

    // Cold path — called from telemetry loop.
    void flush() {
        if (buf_.empty()) return;
        std::ofstream out(path_, std::ios::app);
        if (!out) return;
        out << std::fixed;
        for (const auto& e : buf_) {
            out << e.trade_id << ','
                << e.timestamp_us << ','
                << e.type << ','
                << e.hedge_status << ','
                << e.hl_fill_timestamp_us << ','
                << e.lighter_fill_timestamp_us << ','
                << e.hl_side << ','
                << std::setprecision(6) << e.hl_px << ','
                << std::setprecision(6) << e.hl_sz << ','
                << std::setprecision(8) << e.hl_fee << ','
                << std::setprecision(6) << e.lt_fill_px << ','
                << std::setprecision(6) << e.lt_sz << ','
                << std::setprecision(8) << e.lt_fee << ','
                << std::setprecision(4) << e.spread_bps << ','
                << std::setprecision(4) << e.signal_spread_bps << ','
                << std::setprecision(3) << e.maker_resting_ms << ','
                << std::setprecision(3) << e.hedge_total_ms << ','
                << e.hl_oid << ','
                << e.lt_tx << ','
                << std::setprecision(6) << e.unwind_fill_px << ','
                << std::setprecision(6) << e.unwind_fill_sz << ','
                << std::setprecision(8) << e.unwind_fee << ','
                << e.failure_reason << '\n';
        }
        out.flush();
        buf_.clear();
    }

    [[nodiscard]] std::size_t pending() const noexcept { return buf_.size(); }

  private:
    std::string path_;
    std::vector<JournalEntry> buf_;
};

}  // namespace arb

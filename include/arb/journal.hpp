#pragma once

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace arb {

struct JournalEntry {
    std::int64_t timestamp_us;   // epoch microseconds
    char type;                   // 'T'=trade, 'U'=unwind, 'F'=hedge_fail
    char hl_side;                // 'B'=buy, 'S'=sell
    double hl_px;
    double hl_sz;
    double lt_px;                // 0 if unknown
    double lt_sz;
    double spread_bps;
    std::string hl_oid;
    std::string lt_tx;
};

// Append-only trade journal.  Hot path (record()) only pushes to an
// in-memory vector.  flush() writes buffered entries to disk and is
// called from the telemetry path (every ~500 ticks) so zero I/O
// touches the latency-critical fill→hedge path.
class TradeJournal {
  public:
    explicit TradeJournal(const std::string& path)
        : path_(path) {}

    // Hot path — no I/O, no allocation (vector reserve'd).
    void record(JournalEntry entry) {
        buf_.push_back(std::move(entry));
    }

    // Cold path — called from telemetry loop.
    void flush() {
        if (buf_.empty()) return;
        std::ofstream out(path_, std::ios::app);
        if (!out) return;
        for (const auto& e : buf_) {
            // CSV: ts_us,type,hl_side,hl_px,hl_sz,lt_px,lt_sz,spread_bps,hl_oid,lt_tx
            out << e.timestamp_us << ','
                << e.type << ','
                << e.hl_side << ','
                << e.hl_px << ','
                << e.hl_sz << ','
                << e.lt_px << ','
                << e.lt_sz << ','
                << e.spread_bps << ','
                << e.hl_oid << ','
                << e.lt_tx << '\n';
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

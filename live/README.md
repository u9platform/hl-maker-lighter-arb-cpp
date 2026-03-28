# Live Production Data

This directory is auto-synced from the production server every 5 minutes.
**Last sync timestamp**: see `.last_sync`

## Files

| File | Description |
|------|-------------|
| `trades.csv` | All trades since bot start. Columns: `timestamp_us,type,hl_side,hl_px,hl_sz,lt_px,lt_sz,spread_bps,hl_oid,lt_tx` |
| `arb_live_tail.log` | Last 500 lines of bot log |
| `perf_trace.log` | Performance instrumentation lines (latency breakdown per trade) |
| `status.txt` | Current process status + latest telemetry |
| `config.txt` | Current bot config (non-secret params) |

## Trade CSV Schema

- `timestamp_us`: Unix microseconds
- `type`: `T`=trade complete, `U`=unwind (hedge unconfirmed), `F`=hedge failed
- `hl_side`: `B`=HL bought (long HL / short Lighter), `S`=HL sold
- `hl_px`: HL fill price
- `hl_sz`: HL fill size (base units, e.g. HYPE)
- `lt_px`: Lighter limit price used for hedge (not actual fill price)
- `lt_sz`: Lighter confirmed size
- `spread_bps`: Cross-venue BBO spread at time of trade
- `hl_oid`: Hyperliquid order ID
- `lt_tx`: Lighter transaction hash

## Perf Trace Format

```
perf trade oid=<id> signal_to_hl_send_ms=X hl_send_to_ack_ms=X hl_resting_ms=X hl_fill_rx_to_lt_send_ms=X lt_send_to_ack_ms=X hedge_total_ms=X
perf cancel oid=<id> cancel_trigger_to_send_ms=X cancel_send_to_ack_ms=X hl_resting_ms=X
```

## Notes

- `lt_px` is the limit price, NOT the actual Lighter fill price. Lighter doesn't return fill price in the API response.
- To get actual Lighter PnL, check position delta before/after trade.
- Data refreshes every 5 minutes via `scripts/sync_live_data.sh` (launchd: com.clawd.live-sync)

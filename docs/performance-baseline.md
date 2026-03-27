# Performance Baseline

This repository has two latency domains that should be measured separately:

1. Strategy-core latency
   C++ state-machine execution from `SpreadSnapshot` to emitted action.
2. Venue-integration latency
   The bridge cost of talking to the real HL and Lighter clients.

## What To Measure

- `snapshot_collect_ms`
  Combined time to fetch HL and Lighter BBOs.
- `hl_orderbook_bridge_ms`
  End-to-end latency of `scripts/exchange_bridge.py hl orderbook`.
- `lighter_orderbook_bridge_ms`
  End-to-end latency of `scripts/exchange_bridge.py lighter orderbook`.
- `hl_place_limit_ms`
  Time from C++ action emission to HL place-limit acknowledgement.
- `lighter_ioc_ms`
  Time from HL fill callback to Lighter IOC acknowledgement.
- `hedge_total_ms`
  Time from HL fill callback to either Lighter hedge success or HL unwind start.

## Baseline Procedure

1. Export real credentials and the old Python repo path.
   `export LIGHTER_HL_ARB_SOURCE=/absolute/path/to/lighter-hl-arb`
2. Run bridge-only measurements:
   `python3 scripts/perf_baseline.py --samples 20`
3. Record mean, p50, and max for each command.
4. When the live engine is added, compare:
   - dry-run bridge latency
   - live venue acknowledgement latency
   - hedge total latency after real HL fills

## Initial Expectations

Because the current implementation uses a Python bridge process, this version is not the final low-latency architecture. The purpose of the baseline is to quantify:

- how much overhead the bridge introduces
- whether the real venue clients are stable enough for correctness testing
- which calls should be rewritten in native C++ first

The most likely first native rewrite targets are:

- HL market data
- HL order placement / cancel
- Lighter IOC hedge submission

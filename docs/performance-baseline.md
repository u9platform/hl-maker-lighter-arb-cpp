# Performance Baseline

This repository currently has three latency domains that should be measured separately:

1. Strategy-core latency
   C++ state-machine execution from `SpreadSnapshot` to emitted action.
2. Native public-market-data latency
   Pure C++ HTTP path for HL and Lighter BBO fetches.
3. Authenticated venue-integration latency
   Temporary bridge cost for signed trading actions that are still being ported.

## What To Measure

- `snapshot_collect_ms`
  Combined time to fetch HL and Lighter BBOs.
- `native_hl_orderbook_ms`
  End-to-end latency of `NativeHyperliquidExchange::get_bbo`.
- `native_lighter_orderbook_ms`
  End-to-end latency of `NativeLighterExchange::get_bbo`.
- `hl_orderbook_bridge_ms`
  Temporary comparison number from `scripts/exchange_bridge.py hl orderbook`.
- `lighter_orderbook_bridge_ms`
  Temporary comparison number from `scripts/exchange_bridge.py lighter orderbook`.
- `hl_place_limit_ms`
  Time from C++ action emission to HL place-limit acknowledgement.
- `lighter_ioc_ms`
  Time from HL fill callback to Lighter IOC acknowledgement.
- `hedge_total_ms`
  Time from HL fill callback to either Lighter hedge success or HL unwind start.

## Baseline Procedure

1. Export real credentials and the old Python repo path.
   `export LIGHTER_HL_ARB_SOURCE=/absolute/path/to/lighter-hl-arb`
2. Run native public-market-data measurements:
   `./native_baseline 5`
3. Run bridge-only comparison measurements for the remaining signed path:
   `python3 scripts/perf_baseline.py --samples 20`
4. Record mean, p50, and max for each command.
5. When the live engine is added, compare:
   - dry-run bridge latency
   - live venue acknowledgement latency
   - hedge total latency after real HL fills

## Initial Expectations

Because authenticated trading still uses a temporary bridge for signed calls, this version is not yet the final low-latency architecture. The purpose of the baseline is to quantify:

- how much native market data already improved the hot path
- how much overhead remains in the temporary signed-call bridge
- which authenticated calls should be rewritten in native C++ next

The most likely first native rewrite targets are:

- HL order placement / cancel
- Lighter IOC hedge submission
- persistent native auth and nonce management for Lighter

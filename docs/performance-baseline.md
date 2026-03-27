# Performance Baseline

This repository currently has three latency domains that should be measured separately:

1. Strategy-core latency
   C++ state-machine execution from `SpreadSnapshot` to emitted action.
2. Native public-market-data latency
   Pure C++ HTTP path for HL and Lighter BBO fetches.
3. Native authenticated venue-integration latency
   Pure C++ signed trading path for HL and Lighter.

## What To Measure

- `snapshot_collect_ms`
  Combined time to fetch HL and Lighter BBOs.
- `native_hl_orderbook_ms`
  End-to-end latency of `NativeHyperliquidExchange::get_bbo`.
- `native_lighter_orderbook_ms`
  End-to-end latency of `NativeLighterExchange::get_bbo`.
- `hl_place_limit_ms`
  Time from C++ action emission to HL place-limit acknowledgement.
- `lighter_ioc_ms`
  Time from HL fill callback to Lighter IOC acknowledgement.
- `hedge_total_ms`
  Time from HL fill callback to either Lighter hedge success or HL unwind start.

## Baseline Procedure

1. Export real credentials when you want to benchmark native signed trading.
2. Run native public-market-data measurements:
   `./native_baseline 5`
3. Record mean, p50, and max for each command.
4. When the live engine is added, compare:
   - dry-run native smoke behavior
   - live venue acknowledgement latency
   - hedge total latency after real HL fills

## Initial Expectations

The purpose of the baseline is to quantify:

- how fast the pure C++ market-data path is
- how fast the native signed trading path is once credentials are present
- which path should move from HTTP polling to websocket first

The most likely first native rewrite targets are:

- HL websocket market data
- Lighter websocket market data
- live fill callback handling

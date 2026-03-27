# HL Maker Lighter Arb C++

This project is a fresh C++ rewrite of the execution core for a single strategy:

- watch the HYPE cross-exchange spread between Hyperliquid and Lighter
- when `abs(spread) >= SPREAD`, place a `post-only` maker order on Hyperliquid
- while the order is still resting, cancel it when `abs(spread) < SPREAD - X`
- if the Hyperliquid maker order fills, immediately send a `taker/IOC` hedge to Lighter

The rewrite intentionally excludes the old parallel dual-taker entry path.

## Current Scope

The repository currently contains:

- a small strategy state machine in [`include/arb/strategy.hpp`](./include/arb/strategy.hpp)
- domain types in [`include/arb/types.hpp`](./include/arb/types.hpp)
- exchange adapter interfaces in [`include/arb/exchange.hpp`](./include/arb/exchange.hpp)
- native public market-data clients in [`include/arb/public_exchange.hpp`](./include/arb/public_exchange.hpp)
- native trading clients in [`include/arb/native_trading.hpp`](./include/arb/native_trading.hpp)
- a native execution engine in [`include/arb/engine.hpp`](./include/arb/engine.hpp)
- a lightweight test binary in [`tests/test_strategy.cpp`](./tests/test_strategy.cpp)
- a demo driver in [`src/main.cpp`](./src/main.cpp)

This is now a pure C++ codebase. Runtime trading and market data no longer require Python.

## Build

```bash
cmake -S . -B build
cmake --build build
./build/arb_demo
```

If `cmake` is not installed, the project can still be compiled directly with `clang++` as long as the `include/` and `src/` files are passed together.

## Native Runtime

Current native status:

- native C++:
  - HL public orderbook
  - Lighter public orderbook
- HL signed trading:
  - `place-limit`
  - `cancel`
  - `reduce`
- Lighter signed trading:
  - `place-ioc`

Optional live credentials:

```bash
export HL_PRIVATE_KEY=...
export LIGHTER_API_PRIVATE_KEY=...
export LIGHTER_ACCOUNT_INDEX=0
export LIGHTER_API_KEY_INDEX=0
```

## Tests

The current tests cover:

- maker placement at `SPREAD`
- maker cancel below `SPREAD - X`
- Lighter hedge submission after HL fill
- HL unwind after hedge rejection

Run:

```bash
./arb_tests
```

## Baseline

The measurement plan is documented in [`docs/performance-baseline.md`](./docs/performance-baseline.md).

C++ baseline:

```bash
./native_baseline 5
```

## Architecture Notes

The intended production layering is:

1. `market_data`
   Consumes HL and Lighter order book updates and produces normalized `SpreadSnapshot` events.
2. `strategy`
   Owns the maker lifecycle and emits intents such as `PlaceHlMaker`, `CancelHlMaker`, and `SendLighterTakerHedge`.
3. `execution`
   Converts intents into exchange-specific API calls.
4. `risk`
   Guards exposure, stale quotes, hedge failures, and forced unwind behavior.
5. `telemetry`
   Records timing, fill outcomes, and state transitions without polluting the core state machine.

## Next Steps

- wire live fill callbacks into the strategy state machine
- add websocket market-data clients for HL and Lighter
- harden authenticated native trading with live integration tests
- add deterministic tests for HL/Lighter native signing helpers

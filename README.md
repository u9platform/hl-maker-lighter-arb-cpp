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
- a demo driver in [`src/main.cpp`](./src/main.cpp)

This is the minimum executable scaffold for the new architecture. Exchange adapters, persistence, telemetry, and recovery flows will be added on top of this core.

## Build

```bash
cmake -S . -B build
cmake --build build
./build/arb_demo
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

- add exchange adapter interfaces for HL and Lighter
- wire real fill callbacks into the strategy state machine
- implement unwind and exit handling for open hedged positions
- add deterministic unit tests around threshold transitions and hedge-failure safety

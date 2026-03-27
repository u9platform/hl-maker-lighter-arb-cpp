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
- a bridge-backed execution engine in [`include/arb/engine.hpp`](./include/arb/engine.hpp)
- a Python bridge to the real HL/Lighter clients in [`scripts/exchange_bridge.py`](./scripts/exchange_bridge.py)
- a lightweight test binary in [`tests/test_strategy.cpp`](./tests/test_strategy.cpp)
- a demo driver in [`src/main.cpp`](./src/main.cpp)

This is the minimum executable scaffold for the new architecture. Exchange adapters, persistence, telemetry, and recovery flows will be added on top of this core.

## Build

```bash
cmake -S . -B build
cmake --build build
./build/arb_demo
```

If `cmake` is not installed, the project can still be compiled directly with `clang++` as long as the `include/` and `src/` files are passed together.

## Real Client Bridge

The C++ strategy core talks to real venues through a small Python bridge:

- Hyperliquid:
  - `orderbook`
  - `place-limit`
  - `cancel`
  - `reduce`
- Lighter:
  - `orderbook`
  - `place-ioc`

The bridge intentionally reuses the proven Python clients from the older repository instead of re-implementing live trading logic twice.

Set:

```bash
export LIGHTER_HL_ARB_SOURCE=/absolute/path/to/lighter-hl-arb
```

Optional live credentials:

```bash
export HL_PRIVATE_KEY=...
export HL_ACCOUNT_ADDRESS=...
export LIGHTER_API_PRIVATE_KEY=...
export LIGHTER_ACCOUNT_INDEX=0
export LIGHTER_API_KEY_INDEX=0
```

Suggested local bridge environment:

```bash
python3 -m venv .bridge-venv
./.bridge-venv/bin/pip install -r scripts/bridge-requirements.txt
```

Bridge smoke tests:

```bash
python3 scripts/exchange_bridge.py hl orderbook --coin HYPE
python3 scripts/exchange_bridge.py lighter orderbook --market-id 24
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

Use the baseline helper to measure bridge overhead before optimizing native integrations:

```bash
./.bridge-venv/bin/python scripts/perf_baseline.py --samples 20 --python-bin ./.bridge-venv/bin/python
```

The measurement plan is documented in [`docs/performance-baseline.md`](./docs/performance-baseline.md).

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

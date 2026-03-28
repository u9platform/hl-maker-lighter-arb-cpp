# HL Maker Lighter Arb C++

C++ HFT rewrite of the HL maker / Lighter taker HYPE perpetual futures arbitrage bot.

## Strategy

- Watch the HYPE cross-exchange spread between Hyperliquid and Lighter
- When `abs(spread) >= SPREAD`, place a `post-only` maker order on Hyperliquid
- While the order is resting, cancel if `abs(spread) < SPREAD - CANCEL_BAND`
- On HL maker fill, immediately send a taker/IOC hedge to Lighter

## Architecture

```
┌─────────────────────────────────────────────┐
│              main_live event loop            │
│  CV-driven: wakes on WS BBO update or 100ms │
├─────────┬─────────┬──────────┬──────────────┤
│ Market  │ Fill    │ Risk     │ Strategy +   │
│ Feed    │ Feed    │ Guard    │ Engine       │
│ (WS)    │ (WS)   │          │              │
├─────────┴─────────┴──────────┴──────────────┤
│        WS Exchange Adapters                  │
│  WS BBO reads + native signing for writes    │
├──────────────────────────────────────────────┤
│  WsClient (Boost.Beast TLS WebSocket)        │
│  NativeHyperliquidTrading (EIP712+secp256k1) │
│  NativeLighterTrading (FFI lighter-signer)   │
└──────────────────────────────────────────────┘
```

### Components

| Layer | Files | Description |
|-------|-------|-------------|
| **Market Feed** | `ws_client.hpp/cpp`, `market_feed.hpp/cpp` | Boost.Beast TLS WebSocket with auto-reconnect. HL l2Book + Lighter order_book subscriptions. Thread-safe AtomicBbo |
| **Fill Feed** | `market_feed.hpp/cpp` | HL userFills WS for fill callbacks |
| **Strategy** | `strategy.hpp/cpp` | State machine: Idle → PendingHlMaker → HlFilledPendingLighterHedge → Open |
| **Engine** | `engine.hpp/cpp` | Converts strategy intents to actual trades |
| **Risk** | `risk.hpp/cpp` | Max exposure, stale quote kill switch, hedge failure tracking |
| **WS Adapters** | `ws_exchange.hpp` | WS BBO for reads + native signing for writes (zero REST BBO overhead) |
| **Signing** | `native_trading.hpp/cpp`, `crypto.hpp/cpp` | EIP712+secp256k1 for HL, FFI for Lighter |
| **Event Loop** | `main_live.cpp` | Production: CV-driven ticks, risk checks, telemetry |

## Build

```bash
# Dependencies (macOS)
brew install boost cmake secp256k1

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

## Run

### WebSocket Smoke Test
```bash
./build/ws_smoke     # 30s test, prints live BBO from both venues
```

### Production Bot
```bash
export HL_PRIVATE_KEY=...
export LIGHTER_API_PRIVATE_KEY=...
export LIGHTER_ACCOUNT_INDEX=0
export LIGHTER_API_KEY_INDEX=0
export HL_USER_ADDRESS=0x...    # For fill feed (optional)
export DRY_RUN=true             # Set false for live trading
export SPREAD_BPS=2.0
export CANCEL_BAND_BPS=0.5
export PAIR_SIZE_USD=25.0
export MAX_POSITION_USD=100.0

./build/arb_live
```

### Unit Tests
```bash
./build/arb_tests    # Strategy + engine tests
./build/risk_tests   # Risk guard tests
```

### Performance Baseline
```bash
./build/native_baseline 5
```

## Status

- ✅ Strategy state machine (Idle → PendingHlMaker → HlFilledPendingLighterHedge → Open)
- ✅ Native HL signed trading (place_limit, cancel, reduce via EIP712 + secp256k1)
- ✅ Native Lighter signed trading (place_ioc via FFI to lighter-signer dylib)
- ✅ **WebSocket market data** — HL l2Book + Lighter orderbook, live verified
- ✅ **WebSocket fill feed** — HL userFills for fill callbacks
- ✅ **Risk module** — max exposure, stale quotes, hedge failures, kill switch
- ✅ **Production event loop** — CV-driven, graceful shutdown, telemetry
- ✅ Unit tests (strategy + risk)

## Next Steps

- Live integration testing with real credentials
- Telemetry: latency histograms, fill rate tracking
- Position reconciliation on startup
- HL `bbo` channel subscription (lower latency than l2Book)
- Lighter full book state maintenance for deeper order book analysis

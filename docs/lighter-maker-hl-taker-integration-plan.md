# Lighter Maker -> Hyperliquid Taker Integration Plan

## Current State

As of commit `46451f4`, the codebase contains foundational trading primitives for the reverse path, but the live runtime still executes only:

- `Hyperliquid maker -> Lighter taker`

The following capabilities already exist at the interface or native client level:

- `LighterExchange::place_limit_order(...)`
- `LighterExchange::cancel_order(...)`
- `HyperliquidExchange::place_ioc_order(...)`
- `NativeLighterTrading::set_order_waiter(...)`
- `NativeLighterTrading::set_cancel_waiter(...)`

However, the live strategy path does not yet call them.

## What Is Missing Today

### 1. Strategy state machine

`HlMakerLighterHedger` is still single-path only.

Current actions:

- `PlaceHlMaker`
- `CancelHlMaker`
- `SendLighterTakerHedge`
- `UnwindHlPosition`

Missing reverse-path actions:

- `PlaceLighterMaker`
- `CancelLighterMaker`
- `SendHlTakerHedge`
- `UnwindLighterPosition` or equivalent reverse failure handling

### 2. Engine execution branches

`MakerHedgeEngine::execute_action(...)` only implements:

- HL maker placement
- HL maker cancellation
- Lighter taker hedge
- HL unwind

It does not yet implement:

- Lighter maker placement lifecycle
- Lighter maker cancellation lifecycle
- HL taker hedge after Lighter fill
- Lighter-side failure unwind or residual reconciliation

### 3. Runtime confirmation wiring

`NativeLighterTrading` now safely refuses maker placement/cancellation when confirmations are unavailable. That is good for safety, but it means the reverse path cannot run until the following are wired in `main_live.cpp`:

- `set_order_waiter(...)`
- `set_cancel_waiter(...)`

Without those, Lighter maker placement cannot become a live resting order and Lighter cancel cannot become a confirmed completed cancel.

### 4. Market/fill event sources

The live runtime currently has:

- HL fill feed
- HL trade feed for speculative hedge
- HL/Lighter BBO feeds

But the reverse path additionally needs:

- Lighter resting-order state updates
- Lighter fill events for maker fills
- Lighter cancel confirmation events

The existing `LighterPositionFeed` is not enough for full reverse maker lifecycle management.

### 5. Journal and telemetry schema

Current runtime logs and journal are shaped around:

- `HL maker -> Lighter taker`

Missing reverse-path fields include:

- execution policy / engine id
- Lighter maker order ids
- HL taker tx / ack / fill fields
- reverse-path slippage and latency metrics

## Minimum Viable Reverse Path

The smallest production-usable `Lighter maker -> HL taker` path should be implemented in this order.

### Step 1. Add reverse actions and state

Add action types:

- `PlaceLighterMaker`
- `CancelLighterMaker`
- `SendHlTakerHedge`

Add state variants:

- `PendingLighterMaker`
- `LighterFilledPendingHlHedge`
- `CancelledPendingLighterConfirm`

Either extend the current strategy or introduce a second dedicated strategy class for the reverse path.

### Step 2. Add per-path active order tracking in engine

At minimum the engine needs explicit reverse-path state:

- `active_lighter_order_index`
- `active_lighter_client_order_index`
- `lighter_order_resting_confirmed`
- `lighter_maker_direction`
- `lighter_perf_trace`

This state must not be multiplexed into the current HL maker fields.

### Step 3. Wire Lighter order-state confirmations

Before enabling live maker placement on Lighter, the runtime must provide:

- a waiter that maps `client_order_index -> confirmed resting order`
- a waiter that confirms `order_index` is actually cancelled

This should be sourced from live Lighter order/account streams rather than polling if possible.

### Step 4. Implement execution branches

Add engine branches for:

- `PlaceLighterMaker`
- `CancelLighterMaker`
- `SendHlTakerHedge`

The reverse hedge path should use `HyperliquidExchange::place_ioc_order(...)`.

### Step 5. Add reverse fill handling

Add a runtime path that reacts to Lighter maker fills and triggers:

- HL taker hedge immediately
- risk reconciliation if the fill is partial
- opposite-side order freeze / cancel if dual-maker is enabled later

### Step 6. Add reverse-path journaling

Journal rows must be distinguishable by execution policy. At minimum add:

- `execution_policy`
- `maker_venue`
- `taker_venue`

Recommended values:

- `hl_maker_lt_taker`
- `lt_maker_hl_taker`

## Recommended Runtime Wiring

In `main_live.cpp`, the reverse path should not be enabled until all of the following are true:

- `lighter_native.set_order_waiter(...)` is configured
- `lighter_native.set_cancel_waiter(...)` is configured
- a Lighter fill callback path exists
- HL taker path is enabled and tested

If any of those are missing, the process should either:

- keep the reverse path disabled, or
- run it in shadow mode only

It should not silently pretend the reverse path is active.

## Performance Instrumentation Required

The reverse path must ship with its own metrics from day one.

### Placement path

- `lt_maker_signal_to_send_ms`
- `lt_maker_send_to_ack_ms`
- `lt_maker_send_to_resting_confirm_ms`

### Fill-to-hedge path

- `lt_fill_rx_to_hl_send_ms`
- `hl_taker_send_to_ack_ms`
- `lt_fill_to_hl_ack_total_ms`

### Cancel path

- `lt_cancel_trigger_to_send_ms`
- `lt_cancel_send_to_ack_ms`
- `lt_cancel_send_to_confirm_ms`

### Quality metrics

- `lt_maker_hl_taker_trade_count`
- `lt_maker_hl_taker_net_pnl`
- `lt_maker_hl_taker_win_rate`
- `lt_maker_hl_taker_slippage`

## Rollout Plan

### Phase 1. Shadow wiring

Implement all reverse-path state transitions and logging, but do not send real Lighter maker or HL taker orders.

### Phase 2. Real maker, shadow hedge

Allow:

- real Lighter maker place/cancel

Keep:

- HL taker as shadow-only logging

This validates resting-order confirmation and cancel completion first.

### Phase 3. Small-size live reverse path

Enable real:

- Lighter maker
- HL taker

at small notional only, with policy-isolated metrics.

### Phase 4. Compare and promote

Compare:

- `hl_maker_lt_taker`
- `lt_maker_hl_taker`

on:

- net pnl
- win rate
- slippage
- hedge latency
- residual risk events

Only after that should the system consider running both paths together.

## Short Conclusion

The reverse direction is not failing because of one bug. It is simply not integrated into the live execution spine yet.

The shortest path to make it real is:

1. add reverse strategy actions and state
2. wire Lighter resting/cancel confirmations
3. add Lighter fill -> HL taker execution
4. add reverse-path journaling and perf metrics
5. enable in shadow mode before live rollout

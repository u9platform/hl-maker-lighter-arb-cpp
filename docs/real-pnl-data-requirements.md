# Real PnL Data Requirements

This document defines the minimum data required to upgrade the current `proxy PnL` view into a `real realized PnL` view for the HL maker / Lighter taker strategy.

## Why This Is Needed

The current synced trade data stores:

- Hyperliquid actual fill price and size
- Lighter limit price used for hedge
- Lighter confirmed size

This is enough for a rough proxy, but not enough for true realized PnL, because:

- `lt_px` is the submitted limit price, not the actual Lighter fill price
- fees are not recorded
- unwind / hedge-failure cases are not fully captured as realized PnL events

## Required Fields

These fields are the minimum required to calculate real realized PnL per completed trade.

### Identity And Timing

- `trade_id`
  - Unique ID for one complete trade lifecycle
  - Used to connect HL maker fill, Lighter hedge, and any unwind

- `timestamp_us`
  - Final trade-completion timestamp in Unix microseconds

- `hl_fill_timestamp_us`
  - Hyperliquid fill timestamp in Unix microseconds

- `lighter_fill_timestamp_us`
  - Lighter fill timestamp in Unix microseconds

### Hyperliquid Fill

- `hl_side`
  - `B` for HL buy
  - `S` for HL sell

- `hl_fill_price`
  - Actual HL fill price

- `hl_fill_size`
  - Actual HL fill size in base units

- `hl_fee`
  - Actual HL fee paid for this fill

- `hl_fee_asset`
  - Fee asset, if available

### Lighter Hedge Fill

- `lighter_fill_price`
  - Actual Lighter fill price
  - This is the single most important missing field today

- `lighter_fill_size`
  - Actual Lighter fill size in base units

- `lighter_fee`
  - Actual Lighter fee paid for this fill

- `lighter_fee_asset`
  - Fee asset, if available

### Strategy Context

- `spread_bps`
  - Cross-venue spread observed when the trade was triggered / hedged

- `hl_oid`
  - Hyperliquid order ID

- `lt_tx`
  - Lighter transaction hash

## Strongly Recommended Fields

These are not strictly required for basic net PnL, but they are strongly recommended for debugging and post-trade analysis.

- `hedge_status`
  - Suggested values:
    - `filled`
    - `unconfirmed_then_unwind`
    - `failed`

- `matched_size`
  - Size actually matched between HL and Lighter for PnL pairing

- `signal_spread_bps`
  - Spread when signal fired

- `maker_resting_ms`
  - Time between HL ack and HL fill / cancel

- `hedge_total_ms`
  - Time from HL fill to Lighter hedge completion

## Unwind / Failure Path Fields

If the hedge is unconfirmed or fails, the following fields are needed to compute real realized loss from the failure path.

- `unwind_fill_price`
  - Actual HL unwind fill price

- `unwind_fill_size`
  - Actual HL unwind size

- `unwind_fee`
  - Actual HL unwind fee

- `unwind_fee_asset`
  - Fee asset, if available

- `failure_reason`
  - Short string such as:
    - `lighter_unconfirmed`
    - `lighter_reject`
    - `lighter_timeout`

## Real PnL Formula

For normal completed trades:

- If `hl_side = B`:
  - Gross PnL = `(lighter_fill_price - hl_fill_price) * matched_size`

- If `hl_side = S`:
  - Gross PnL = `(hl_fill_price - lighter_fill_price) * matched_size`

- Net PnL:
  - `gross_pnl - hl_fee - lighter_fee`

For unwind / failure-path trades:

- Net PnL must be computed from the actual unwind fill and unwind fee
- These should be tracked separately from normal completed hedge trades

## Minimum CSV Schema

If the production sync continues to use CSV, the minimum useful schema should be:

```csv
trade_id,timestamp_us,type,hl_side,hl_fill_price,hl_fill_size,lighter_fill_price,lighter_fill_size,hl_fee,lighter_fee,spread_bps,hl_oid,lt_tx
```

## Recommended CSV Schema

If we also want proper failure-path accounting:

```csv
trade_id,timestamp_us,type,hedge_status,hl_fill_timestamp_us,lighter_fill_timestamp_us,hl_side,hl_fill_price,hl_fill_size,lighter_fill_price,lighter_fill_size,matched_size,hl_fee,hl_fee_asset,lighter_fee,lighter_fee_asset,unwind_fill_price,unwind_fill_size,unwind_fee,unwind_fee_asset,failure_reason,spread_bps,signal_spread_bps,maker_resting_ms,hedge_total_ms,hl_oid,lt_tx
```

## Short-Term Recommendation

The fastest path to real realized PnL is to add these first:

1. `trade_id`
2. `lighter_fill_price`
3. `hl_fee`
4. `lighter_fee`
5. `hedge_status`
6. `unwind_fill_price`
7. `unwind_fee`

If only one field can be added first, add:

- `lighter_fill_price`

Without it, every current PnL calculation remains a proxy.

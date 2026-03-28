#!/usr/bin/env python3
"""
Grid search for optimal spread parameters.
Goal: 0 profit, maximum trade volume.

Uses telem log data to simulate the strategy:
- When |spread| >= open_spread_bps → open trade (if flat or same direction)
- When |spread| >= close_spread_bps → close trade (if reducing position)
- Each trade: maker on HL + taker on Lighter
- PnL per trade = |spread_bps| * pair_size / 10000 - fees

Fee model:
- HL maker: -0.02% (rebate) on HYPE
- Lighter taker: 0% (current)
- Slippage: configurable (default 0.5 bps)
"""

import re
import sys
from dataclasses import dataclass
from itertools import product

# Fee assumptions
HL_MAKER_FEE_BPS = 1.5    # 0.015% maker fee = 1.5 bps
LIGHTER_TAKER_FEE_BPS = 0.0  # 0% currently
SLIPPAGE_BPS = 0.5  # estimated execution slippage

# Future fee after 14d $5M volume + 100 HYPE staked
# HL_MAKER_FEE_BPS = 1.08  # 0.0108%

@dataclass
class Tick:
    timestamp: str
    spread_bps: float
    hl_mid: float
    lt_mid: float

def parse_telem(filepath: str) -> list[Tick]:
    ticks = []
    pattern = re.compile(
        r'\[telem\]\s+(\S+)\s+.*spread=([-\d.]+)bps\s+hl=([\d.]+)/([\d.]+).*lt=([\d.]+)/([\d.]+)'
    )
    with open(filepath) as f:
        for line in f:
            m = pattern.search(line)
            if m:
                ts = m.group(1)
                spread = float(m.group(2))
                hl_bid, hl_ask = float(m.group(3)), float(m.group(4))
                lt_bid, lt_ask = float(m.group(5)), float(m.group(6))
                ticks.append(Tick(
                    timestamp=ts,
                    spread_bps=spread,
                    hl_mid=(hl_bid + hl_ask) / 2,
                    lt_mid=(lt_bid + lt_ask) / 2,
                ))
    return ticks

def simulate(ticks: list[Tick], open_bps: float, close_bps: float, 
             cancel_band_bps: float, pair_size_usd: float, max_pos_usd: float) -> dict:
    """Simulate trading with given parameters. Returns stats dict."""
    position = 0.0  # in base units, positive = long HL
    total_pnl = 0.0
    total_volume_usd = 0.0
    num_trades = 0
    num_opens = 0
    num_closes = 0
    num_winning = 0
    num_losing = 0
    winning_pnl = 0.0
    losing_pnl = 0.0
    max_position_usd_seen = 0.0
    
    # Total cost per round-trip: HL maker fee + Lighter taker fee + slippage
    # All positive = you pay
    total_fee_bps = HL_MAKER_FEE_BPS + LIGHTER_TAKER_FEE_BPS + SLIPPAGE_BPS
    
    for tick in ticks:
        abs_spread = abs(tick.spread_bps)
        mid = (tick.hl_mid + tick.lt_mid) / 2
        if mid <= 0:
            continue
            
        pos_usd = abs(position) * mid
        max_position_usd_seen = max(max_position_usd_seen, pos_usd)
        
        # Determine trade direction
        # spread > 0 → LT > HL → buy HL, sell LT (ShortLighterLongHl)
        # spread < 0 → HL > LT → sell HL, buy LT (LongLighterShortHl)
        would_buy_hl = tick.spread_bps >= 0
        
        # Would this reduce position?
        would_reduce = (position < 0 and would_buy_hl) or (position > 0 and not would_buy_hl)
        
        # Determine threshold
        if would_reduce and abs(position) > 0.01:
            threshold = close_bps
        else:
            threshold = open_bps
            
        # Position limit check (only for increasing)
        if not would_reduce and pos_usd >= max_pos_usd:
            continue
            
        if abs_spread >= threshold:
            trade_size_base = pair_size_usd / mid
            
            # PnL: spread captured minus costs
            net_spread_bps = abs_spread - total_fee_bps
            trade_pnl = net_spread_bps * pair_size_usd / 10000
            
            total_pnl += trade_pnl
            total_volume_usd += pair_size_usd
            num_trades += 1
            if trade_pnl >= 0:
                num_winning += 1
                winning_pnl += trade_pnl
            else:
                num_losing += 1
                losing_pnl += trade_pnl
            
            if would_reduce:
                num_closes += 1
                if would_buy_hl:
                    position += trade_size_base
                else:
                    position -= trade_size_base
            else:
                num_opens += 1
                if would_buy_hl:
                    position += trade_size_base
                else:
                    position -= trade_size_base
    
    return {
        'open_bps': open_bps,
        'close_bps': close_bps,
        'cancel_band_bps': cancel_band_bps,
        'num_trades': num_trades,
        'num_opens': num_opens,
        'num_closes': num_closes,
        'num_winning': num_winning,
        'num_losing': num_losing,
        'total_pnl': total_pnl,
        'winning_pnl': winning_pnl,
        'losing_pnl': losing_pnl,
        'total_volume_usd': total_volume_usd,
        'final_pos_base': position,
        'max_pos_usd': max_position_usd_seen,
        'pnl_per_trade': total_pnl / num_trades if num_trades > 0 else 0,
    }

def main():
    filepath = sys.argv[1] if len(sys.argv) > 1 else '/tmp/telem_data.txt'
    ticks = parse_telem(filepath)
    print(f"Loaded {len(ticks)} ticks from {ticks[0].timestamp} to {ticks[-1].timestamp}")
    
    # Spread distribution
    spreads = [abs(t.spread_bps) for t in ticks]
    spreads.sort()
    print(f"\nSpread distribution (abs):")
    for pct in [25, 50, 75, 90, 95, 99]:
        idx = int(len(spreads) * pct / 100)
        print(f"  P{pct}: {spreads[idx]:.2f} bps")
    print(f"  Mean: {sum(spreads)/len(spreads):.2f} bps")
    print(f"  Max: {max(spreads):.2f} bps")
    
    # Grid search
    pair_size = 25.0
    max_pos = 100.0
    
    open_range = [x * 0.5 for x in range(1, 21)]    # 0.5 to 10.0
    close_range = [x * 0.5 for x in range(1, 21)]   # 0.5 to 10.0
    cancel_band = 0.5  # fixed for now
    
    results = []
    for open_bps in open_range:
        for close_bps in close_range:
            if close_bps > open_bps:
                continue  # close threshold should be <= open threshold
            r = simulate(ticks, open_bps, close_bps, cancel_band, pair_size, max_pos)
            results.append(r)
    
    # Sort by: PnL >= 0, then max trades
    profitable = [r for r in results if r['total_pnl'] >= 0 and r['num_trades'] > 0]
    profitable.sort(key=lambda r: -r['num_trades'])
    
    print(f"\n{'='*100}")
    print(f"Grid Search Results (PnL >= 0, sorted by max trades)")
    print(f"{'='*100}")
    print(f"{'Open':>6} {'Close':>6} {'Trades':>7} {'Opens':>6} {'Closes':>7} {'Volume':>10} {'PnL':>10} {'PnL/Trade':>10} {'FinalPos':>10}")
    print(f"{'(bps)':>6} {'(bps)':>6} {'':>7} {'':>6} {'':>7} {'(USD)':>10} {'(USD)':>10} {'(USD)':>10} {'(base)':>10}")
    print('-' * 100)
    
    for r in profitable[:30]:
        print(f"{r['open_bps']:>6.1f} {r['close_bps']:>6.1f} {r['num_trades']:>7} {r['num_opens']:>6} {r['num_closes']:>7} "
              f"{r['total_volume_usd']:>10.0f} {r['total_pnl']:>10.4f} {r['pnl_per_trade']:>10.4f} {r['final_pos_base']:>10.2f}")
    
    # Also show the all results sorted by trades (including negative PnL)
    all_sorted = sorted(results, key=lambda r: -r['num_trades'])
    print(f"\n{'='*100}")
    print(f"All Results (top 20 by trades, including negative PnL)")
    print(f"{'='*100}")
    print(f"{'Open':>6} {'Close':>6} {'Trades':>7} {'Opens':>6} {'Closes':>7} {'Volume':>10} {'PnL':>10} {'PnL/Trade':>10} {'FinalPos':>10}")
    print('-' * 100)
    for r in all_sorted[:20]:
        marker = "✅" if r['total_pnl'] >= 0 else "❌"
        print(f"{r['open_bps']:>6.1f} {r['close_bps']:>6.1f} {r['num_trades']:>7} {r['num_opens']:>6} {r['num_closes']:>7} "
              f"{r['total_volume_usd']:>10.0f} {r['total_pnl']:>10.4f} {r['pnl_per_trade']:>10.4f} {r['final_pos_base']:>10.2f} {marker}")

if __name__ == '__main__':
    main()

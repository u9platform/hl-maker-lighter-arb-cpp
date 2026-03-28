#!/usr/bin/env python3
"""Detail analysis for a specific parameter set."""
import re, sys

TOTAL_FEE_BPS = float(sys.argv[4]) if len(sys.argv) > 4 else 4.0  # default 4bps round-trip cost

def parse_telem(filepath):
    ticks = []
    pattern = re.compile(
        r'\[telem\]\s+(\S+)\s+.*spread=([-\d.]+)bps\s+hl=([\d.]+)/([\d.]+).*lt=([\d.]+)/([\d.]+)'
    )
    with open(filepath) as f:
        for line in f:
            m = pattern.search(line)
            if m:
                ticks.append({
                    'ts': m.group(1),
                    'spread': float(m.group(2)),
                    'hl_mid': (float(m.group(3)) + float(m.group(4))) / 2,
                    'lt_mid': (float(m.group(5)) + float(m.group(6))) / 2,
                })
    return ticks

def simulate_detail(ticks, open_bps, close_bps, pair_size=25.0, max_pos=100.0):
    position = 0.0
    trades = []
    
    for tick in ticks:
        abs_spread = abs(tick['spread'])
        mid = (tick['hl_mid'] + tick['lt_mid']) / 2
        if mid <= 0: continue
        
        pos_usd = abs(position) * mid
        would_buy_hl = tick['spread'] >= 0
        would_reduce = (position < 0 and would_buy_hl) or (position > 0 and not would_buy_hl)
        
        threshold = close_bps if (would_reduce and abs(position) > 0.01) else open_bps
        
        if not would_reduce and pos_usd >= max_pos:
            continue
            
        if abs_spread >= threshold:
            net_bps = abs_spread - TOTAL_FEE_BPS
            pnl = net_bps * pair_size / 10000
            trade_type = "CLOSE" if would_reduce else "OPEN"
            
            trades.append({
                'ts': tick['ts'],
                'spread': tick['spread'],
                'abs_spread': abs_spread,
                'threshold': threshold,
                'net_bps': net_bps,
                'pnl': pnl,
                'type': trade_type,
            })
            
            size_base = pair_size / mid
            if would_buy_hl:
                position += size_base
            else:
                position -= size_base
    
    return trades

filepath = sys.argv[1] if len(sys.argv) > 1 else '/tmp/telem_data.txt'
open_bps = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
close_bps = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5

ticks = parse_telem(filepath)
trades = simulate_detail(ticks, open_bps, close_bps)

print(f"Parameters: open={open_bps} close={close_bps} fee={TOTAL_FEE_BPS}bps")
print(f"Total trades: {len(trades)}")
print()

# Spread distribution of executed trades
spreads = [t['abs_spread'] for t in trades]
winning = [t for t in trades if t['pnl'] >= 0]
losing = [t for t in trades if t['pnl'] < 0]

print(f"Winning trades: {len(winning)} (spread >= {TOTAL_FEE_BPS}bps)")
print(f"Losing trades:  {len(losing)} (spread < {TOTAL_FEE_BPS}bps)")
print()

total_pnl = sum(t['pnl'] for t in trades)
win_pnl = sum(t['pnl'] for t in winning)
lose_pnl = sum(t['pnl'] for t in losing)
print(f"Total PnL:   ${total_pnl:.4f}")
print(f"Win PnL:     ${win_pnl:.4f}")
print(f"Lose PnL:    ${lose_pnl:.4f}")
print()

# Spread histogram for executed trades
print("Spread distribution of executed trades:")
buckets = [(0, 1), (1, 2), (2, 3), (3, 4), (4, 5), (5, 10), (10, 50), (50, 500)]
for lo, hi in buckets:
    count = sum(1 for s in spreads if lo <= s < hi)
    pnl = sum(t['pnl'] for t in trades if lo <= t['abs_spread'] < hi)
    if count > 0:
        print(f"  [{lo:>3}-{hi:>3}) bps: {count:>4} trades, PnL=${pnl:>8.4f}, avg_spread={sum(t['abs_spread'] for t in trades if lo <= t['abs_spread'] < hi)/count:.2f}bps")

print()
print("Break-even spread = {:.1f} bps".format(TOTAL_FEE_BPS))
print(f"Trades below break-even: {len(losing)} / {len(trades)} ({100*len(losing)/len(trades):.1f}%)")
print(f"  → These trades LOSE money individually but increase volume")

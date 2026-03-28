#!/usr/bin/env python3
"""
Grid search v2: with configurable round-trip cost.
Goal: maximize volume while PnL >= 0.
"""
import re, sys
from itertools import product

TOTAL_FEE_BPS = float(sys.argv[2]) if len(sys.argv) > 2 else 4.0

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
                    'spread': float(m.group(2)),
                    'hl_mid': (float(m.group(3)) + float(m.group(4))) / 2,
                    'lt_mid': (float(m.group(5)) + float(m.group(6))) / 2,
                })
    return ticks

def simulate(ticks, open_bps, close_bps, pair_size=25.0, max_pos=100.0):
    position = 0.0
    total_pnl = 0.0
    total_vol = 0.0
    n_trades = 0
    n_win = 0
    n_lose = 0
    
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
            total_pnl += pnl
            total_vol += pair_size
            n_trades += 1
            if pnl >= 0: n_win += 1
            else: n_lose += 1
            
            size_base = pair_size / mid
            if would_buy_hl: position += size_base
            else: position -= size_base
    
    return {
        'open': open_bps, 'close': close_bps,
        'trades': n_trades, 'win': n_win, 'lose': n_lose,
        'volume': total_vol, 'pnl': total_pnl,
        'pnl_per_trade': total_pnl / n_trades if n_trades else 0,
        'final_pos': position,
        'win_rate': n_win / n_trades * 100 if n_trades else 0,
    }

filepath = sys.argv[1] if len(sys.argv) > 1 else '/tmp/telem_data.txt'
ticks = parse_telem(filepath)
hours = len(ticks) * 7 / 3600  # ~7s per tick
print(f"Loaded {len(ticks)} ticks (~{hours:.1f}h), fee={TOTAL_FEE_BPS}bps")

# Fine-grained grid: 0.5 to 10.0 step 0.5
vals = [x * 0.5 for x in range(1, 21)]
results = []
for o in vals:
    for c in vals:
        if c > o: continue
        r = simulate(ticks, o, c)
        if r['trades'] > 0:
            results.append(r)

# Sort by: PnL >= 0, then max volume
profitable = [r for r in results if r['pnl'] >= -0.01]  # allow tiny float error
profitable.sort(key=lambda r: -r['volume'])

print(f"\n{'='*110}")
print(f"Top 30 by volume (PnL >= 0) | fee = {TOTAL_FEE_BPS}bps")
print(f"{'='*110}")
print(f"{'Open':>6} {'Close':>6} {'Trades':>7} {'Win':>5} {'Lose':>5} {'WinR%':>6} {'Volume':>10} {'PnL':>10} {'$/trade':>10} {'FinalPos':>10}")
print('-' * 110)
for r in profitable[:30]:
    print(f"{r['open']:>6.1f} {r['close']:>6.1f} {r['trades']:>7} {r['win']:>5} {r['lose']:>5} {r['win_rate']:>5.1f}% "
          f"{r['volume']:>10.0f} {r['pnl']:>10.4f} {r['pnl_per_trade']:>10.4f} {r['final_pos']:>10.2f}")

# Also show: what if we exclude the outlier (356bps)?
print(f"\n{'='*110}")
print(f"Same but EXCLUDING spreads > 50bps (outlier filter)")
print(f"{'='*110}")
ticks_clean = [t for t in ticks if abs(t['spread']) <= 50]
results2 = []
for o in vals:
    for c in vals:
        if c > o: continue
        r = simulate(ticks_clean, o, c)
        if r['trades'] > 0:
            results2.append(r)
profitable2 = [r for r in results2 if r['pnl'] >= -0.01]
profitable2.sort(key=lambda r: -r['volume'])
print(f"{'Open':>6} {'Close':>6} {'Trades':>7} {'Win':>5} {'Lose':>5} {'WinR%':>6} {'Volume':>10} {'PnL':>10} {'$/trade':>10}")
print('-' * 110)
for r in profitable2[:30]:
    print(f"{r['open']:>6.1f} {r['close']:>6.1f} {r['trades']:>7} {r['win']:>5} {r['lose']:>5} {r['win_rate']:>5.1f}% "
          f"{r['volume']:>10.0f} {r['pnl']:>10.4f} {r['pnl_per_trade']:>10.4f}")

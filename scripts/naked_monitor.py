#!/usr/bin/env python3
"""
Naked position monitor for HL-Lighter arb bot.
Every 30s: query HL position via REST + parse bot log for Lighter trades count.
If HL has position but bot shows trades=0 or pos changed without trade logged,
that's a naked position. Confirmed 2x in a row → auto-close on HL.

More precisely: HL actual position vs "expected" position tracked from bot fills.
Since Lighter REST is 403'd, we compare HL position with what the bot THINKS its
position is (from telem log). Any divergence = missed fill = naked.

Usage:
    python3 naked_monitor.py

Env vars (from .env):
    HL_USER_ADDRESS, HL_PRIVATE_KEY
"""

import json, os, re, sys, time, struct, subprocess
import requests
from pathlib import Path
from datetime import datetime, timezone, timedelta

JST = timedelta(hours=9)
CHECK_INTERVAL = 30
MISMATCH_THRESHOLD = 0.05

LOG_FILE = "/tmp/arb_live.log"

def ts():
    return datetime.now(timezone(JST)).strftime("%H:%M:%S")

def log(msg):
    print(f"[naked-monitor] {ts()} {msg}", flush=True)

def load_env(path=None):
    p = path or Path(__file__).parent.parent / ".env"
    if p.exists():
        for line in p.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            os.environ.setdefault(k.strip(), v.strip())

# ── HL position (REST) ──────────────────────────────────────────

def get_hl_position(address: str, coin: str = "HYPE") -> float | None:
    try:
        r = requests.post(
            "https://api.hyperliquid.xyz/info",
            json={"type": "clearinghouseState", "user": address},
            timeout=10,
        )
        if r.status_code != 200:
            return None
        data = r.json()
        for ap in data.get("assetPositions", []):
            p = ap.get("position", {})
            if p.get("coin") == coin:
                return float(p.get("szi", "0"))
        return 0.0
    except Exception as e:
        log(f"HL query error: {e}")
        return None

# ── Bot's internal position (from telem log) ─────────────────────

def get_bot_position_from_log() -> float | None:
    """Parse the latest telem line from bot log to get engine's pos."""
    try:
        # Get last 50 lines, find latest telem
        result = subprocess.run(
            ["tail", "-50", LOG_FILE],
            capture_output=True, text=True, timeout=5,
        )
        lines = result.stdout.strip().split("\n")
        for line in reversed(lines):
            if "[telem]" in line and "pos=" in line:
                m = re.search(r'pos=([-\d.]+)', line)
                if m:
                    return float(m.group(1))
        return None
    except Exception as e:
        log(f"Log parse error: {e}")
        return None

def get_bot_trades_from_log() -> int | None:
    """Get current trades count from bot telem."""
    try:
        result = subprocess.run(
            ["tail", "-50", LOG_FILE],
            capture_output=True, text=True, timeout=5,
        )
        lines = result.stdout.strip().split("\n")
        for line in reversed(lines):
            if "[telem]" in line and "trades=" in line:
                m = re.search(r'trades=(\d+)', line)
                if m:
                    return int(m.group(1))
        return None
    except:
        return None

# ── HL close order ───────────────────────────────────────────────

def close_hl_position(address: str, private_key: str, coin: str, size_to_close: float) -> bool:
    """
    Close `size_to_close` on HL using IOC reduce-only.
    size_to_close > 0 → sell (reduce long)
    size_to_close < 0 → buy (reduce short)
    """
    try:
        from eth_account import Account
        from web3 import Web3
        import msgpack

        is_buy = size_to_close < 0
        abs_size = round(abs(size_to_close), 2)
        if abs_size < 0.01:
            log(f"Size too small: {abs_size}")
            return True

        # Mid price
        r = requests.post(
            "https://api.hyperliquid.xyz/info",
            json={"type": "allMids"},
            timeout=10,
        )
        mid = float(r.json().get(coin, "0"))
        if mid <= 0:
            log("Cannot get mid price")
            return False

        # Aggressive price
        price = round(mid * (1.02 if is_buy else 0.98), 5)
        if mid > 10:
            price = round(price, 2)
        elif mid > 1:
            price = round(price, 3)

        # Asset index
        r2 = requests.post(
            "https://api.hyperliquid.xyz/info",
            json={"type": "meta"},
            timeout=10,
        )
        asset_idx = 0
        for i, u in enumerate(r2.json().get("universe", [])):
            if u.get("name") == coin:
                asset_idx = i
                break

        log(f"Closing: {'BUY' if is_buy else 'SELL'} {abs_size} {coin} @ {price} (IOC reduce-only)")

        order = {
            "a": asset_idx,
            "b": is_buy,
            "p": str(price),
            "s": str(abs_size),
            "r": True,
            "t": {"limit": {"tif": "Ioc"}},
        }
        action = {"type": "order", "orders": [order], "grouping": "na"}
        nonce = int(time.time() * 1000)

        action_bytes = msgpack.packb(action, use_bin_type=True)
        nonce_bytes = struct.pack(">Q", nonce)
        connection_id = Web3.keccak(action_bytes + nonce_bytes + b"\x00")

        account = Account.from_key(private_key)
        signed = account.sign_typed_data(
            {"name": "Exchange", "version": "1", "chainId": 1337,
             "verifyingContract": "0x0000000000000000000000000000000000000000"},
            {"Agent": [{"name": "source", "type": "string"},
                       {"name": "connectionId", "type": "bytes32"}]},
            {"source": "a", "connectionId": connection_id},
        )

        payload = {
            "action": action,
            "nonce": nonce,
            "signature": {"r": hex(signed.r), "s": hex(signed.s), "v": signed.v},
            "vaultAddress": None,
        }

        r3 = requests.post("https://api.hyperliquid.xyz/exchange", json=payload, timeout=15)
        if r3.status_code == 200:
            result = r3.json()
            log(f"Result: {json.dumps(result)}")
            if result.get("status") == "ok":
                statuses = result.get("response", {}).get("data", {}).get("statuses", [])
                for s in statuses:
                    if "filled" in s:
                        f = s["filled"]
                        log(f"✅ Filled {f.get('totalSz')} @ {f.get('avgPx')}")
                        return True
                    elif "error" in s:
                        log(f"❌ {s['error']}")
                        return False
            return False
        else:
            log(f"HTTP {r3.status_code}: {r3.text[:200]}")
            return False
    except Exception as e:
        log(f"Close error: {e}")
        import traceback; traceback.print_exc()
        return False

# ── Main ─────────────────────────────────────────────────────────

def main():
    load_env()

    hl_address = os.environ.get("HL_USER_ADDRESS", "")
    hl_key = os.environ.get("HL_PRIVATE_KEY", "")
    coin = os.environ.get("HL_COIN", "HYPE")

    if not hl_address:
        log("ERROR: HL_USER_ADDRESS not set")
        sys.exit(1)

    log(f"Started | HL={hl_address[:10]}... | interval={CHECK_INTERVAL}s")

    prev_mismatch = None  # (hl_actual, bot_pos, diff)

    while True:
        try:
            hl_actual = get_hl_position(hl_address, coin)
            bot_pos = get_bot_position_from_log()

            if hl_actual is None:
                log("HL API unavailable, skip")
            elif bot_pos is None:
                log("Bot log unavailable, skip")
            else:
                # Compare: HL actual vs what bot thinks
                # If bot missed a fill, bot_pos is stale but HL actual reflects the fill.
                diff = hl_actual - bot_pos

                if abs(diff) <= MISMATCH_THRESHOLD:
                    if prev_mismatch is not None:
                        log(f"✅ Resolved. HL={hl_actual:.4f} bot={bot_pos:.4f}")
                        prev_mismatch = None
                    else:
                        log(f"OK HL={hl_actual:.4f} bot={bot_pos:.4f}")
                else:
                    log(f"⚠️  MISMATCH HL={hl_actual:.4f} bot={bot_pos:.4f} diff={diff:+.4f}")

                    if prev_mismatch is not None:
                        prev_diff = prev_mismatch[2]
                        if abs(diff - prev_diff) < MISMATCH_THRESHOLD:
                            log(f"🚨 CONFIRMED x2: diff={diff:+.4f} (prev={prev_diff:+.4f})")

                            # The HL position is the ground truth.
                            # diff > 0 means HL has MORE than bot thinks (untracked long/less short)
                            # diff < 0 means HL has LESS than bot thinks (untracked short/less long)
                            # We need to close the UNHEDGED part on HL.
                            # The unhedged exposure = diff
                            # If diff=+0.63 → HL bought 0.63 that bot didn't hedge on Lighter
                            # → need to SELL 0.63 on HL to flatten
                            # If diff=-0.63 → HL sold 0.63 unhedged → BUY 0.63 on HL

                            if hl_key:
                                log(f"Auto-closing diff={diff:+.4f} on HL")
                                # close_size positive = sell, negative = buy
                                success = close_hl_position(hl_address, hl_key, coin, diff)
                                if success:
                                    log("Sent. Will verify next check.")
                                else:
                                    log("FAILED. Will retry.")
                            else:
                                log("No HL_PRIVATE_KEY, manual close needed!")

                            prev_mismatch = None
                        else:
                            log(f"Diff changed ({prev_diff:+.4f}→{diff:+.4f}), update")
                            prev_mismatch = (hl_actual, bot_pos, diff)
                    else:
                        log(f"First mismatch, confirming in {CHECK_INTERVAL}s...")
                        prev_mismatch = (hl_actual, bot_pos, diff)

        except Exception as e:
            log(f"Error: {e}")

        time.sleep(CHECK_INTERVAL)


if __name__ == "__main__":
    main()

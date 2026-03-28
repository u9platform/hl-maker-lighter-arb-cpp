#!/usr/bin/env python3
"""
Naked position monitor for HL-Lighter arb bot.
Checks every 30s by comparing HL actual position (REST API) with
Lighter position (WS subscription). If mismatch confirmed twice
in a row (same direction, same magnitude), auto-closes HL excess.

Usage:
    python3 naked_monitor.py          # run forever
    python3 naked_monitor.py --once   # single check

Env vars (from .env):
    HL_USER_ADDRESS, HL_PRIVATE_KEY, LIGHTER_ACCOUNT_INDEX
"""

import json, os, sys, time, struct, threading
import requests
import websocket  # pip install websocket-client
from pathlib import Path
from datetime import datetime, timezone, timedelta

JST = timedelta(hours=9)
CHECK_INTERVAL = 30
MISMATCH_THRESHOLD = 0.05  # base units

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

# ── HL position (REST) ──────────────────────────────────────────────

def get_hl_position(address: str, coin: str = "HYPE") -> float | None:
    try:
        r = requests.post(
            "https://api.hyperliquid.xyz/info",
            json={"type": "clearinghouseState", "user": address},
            timeout=10,
        )
        if r.status_code != 200:
            log(f"HL API {r.status_code}")
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

# ── Lighter position (WS) ───────────────────────────────────────────

class LighterPositionWatcher:
    """Subscribe to Lighter WS for account positions."""

    def __init__(self, account_index: int):
        self.account_index = account_index
        self.position = None  # latest known position (float)
        self._lock = threading.Lock()
        self._ws = None
        self._thread = None
        self._running = False

    def start(self):
        self._running = True
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._running = False
        if self._ws:
            try:
                self._ws.close()
            except:
                pass

    def get_position(self) -> float | None:
        with self._lock:
            return self.position

    def _run(self):
        while self._running:
            try:
                self._connect()
            except Exception as e:
                log(f"Lighter WS error: {e}")
            if self._running:
                time.sleep(5)

    def _connect(self):
        url = "wss://mainnet.zklighter.elliot.ai/stream"
        log(f"Connecting to Lighter WS for account {self.account_index}...")

        ws = websocket.WebSocket()
        ws.connect(url, timeout=15)
        self._ws = ws

        # Subscribe to positions (no auth needed for read-only)
        sub = json.dumps({
            "type": "subscribe",
            "channel": f"account_all_positions/{self.account_index}",
        })
        ws.send(sub)
        log("Subscribed to Lighter positions")

        while self._running:
            try:
                ws.settimeout(60)
                msg = ws.recv()
                if not msg:
                    break
                self._handle_message(msg)
            except websocket.WebSocketTimeoutException:
                # Send ping to keep alive
                try:
                    ws.ping()
                except:
                    break
            except Exception as e:
                log(f"Lighter WS recv error: {e}")
                break

        try:
            ws.close()
        except:
            pass

    def _handle_message(self, raw: str):
        try:
            data = json.loads(raw)
        except:
            return

        # Handle subscription confirmation
        if "subscribed" in str(data.get("channel", "")):
            log(f"Lighter WS confirmed: {data.get('channel')}")
            return

        # Handle position updates
        # Format: {"channel": "account_all_positions:XXXX", "data": {"positions": [...]}}
        channel = data.get("channel", "")
        if "account_all_positions" not in channel:
            return

        positions = data.get("data", {}).get("positions", [])
        total = 0.0
        for p in positions:
            # Lighter position: positive = long, negative = short
            size = float(p.get("size", "0"))
            total += size

        with self._lock:
            self.position = total

# ── HL close (reduce-only IOC) ──────────────────────────────────────

def close_hl_position_simple(address: str, private_key: str, coin: str, net_exposure: float):
    """
    Close net exposure on HL.
    net_exposure > 0 means HL is over-long → sell on HL
    net_exposure < 0 means HL is over-short → buy on HL
    Uses the Python SDK approach via direct API.
    """
    try:
        from eth_account import Account
        from web3 import Web3
        import msgpack

        is_buy = net_exposure < 0  # over-short → buy
        abs_size = round(abs(net_exposure), 2)
        if abs_size < 0.01:
            log(f"Exposure too small to close: {abs_size}")
            return True

        # Get mid price
        r = requests.post(
            "https://api.hyperliquid.xyz/info",
            json={"type": "allMids"},
            timeout=10,
        )
        mids = r.json()
        mid = float(mids.get(coin, "0"))
        if mid <= 0:
            log(f"Cannot get mid price")
            return False

        # Aggressive IOC price
        slippage = 1.02 if is_buy else 0.98
        price = round(mid * slippage, 5)
        # Round to significant price tick
        if mid > 10:
            price = round(price, 2)
        elif mid > 1:
            price = round(price, 3)
        else:
            price = round(price, 5)

        # Get asset index
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

        log(f"Closing HL: {'BUY' if is_buy else 'SELL'} {abs_size} {coin} @ {price} (IOC reduce-only)")

        # Build the order action
        order = {
            "a": asset_idx,
            "b": is_buy,
            "p": str(price),
            "s": str(abs_size),
            "r": True,  # reduce only
            "t": {"limit": {"tif": "Ioc"}},
        }
        action = {
            "type": "order",
            "orders": [order],
            "grouping": "na",
        }

        nonce = int(time.time() * 1000)

        # Hash: msgpack(action) + nonce(8 bytes BE) + vault_flag(1 byte)
        action_bytes = msgpack.packb(action, use_bin_type=True)
        nonce_bytes = struct.pack(">Q", nonce)
        vault_byte = b"\x00"
        connection_id = Web3.keccak(action_bytes + nonce_bytes + vault_byte)

        # EIP-712 sign
        account = Account.from_key(private_key)
        domain = {
            "name": "Exchange",
            "version": "1",
            "chainId": 1337,
            "verifyingContract": "0x0000000000000000000000000000000000000000",
        }
        types = {
            "Agent": [
                {"name": "source", "type": "string"},
                {"name": "connectionId", "type": "bytes32"},
            ]
        }
        message = {
            "source": "a",
            "connectionId": connection_id,
        }
        signed = account.sign_typed_data(domain, types, message)

        payload = {
            "action": action,
            "nonce": nonce,
            "signature": {
                "r": hex(signed.r),
                "s": hex(signed.s),
                "v": signed.v,
            },
            "vaultAddress": None,
        }

        r3 = requests.post(
            "https://api.hyperliquid.xyz/exchange",
            json=payload,
            timeout=15,
        )

        if r3.status_code == 200:
            result = r3.json()
            log(f"HL close result: {json.dumps(result)}")
            status = result.get("status", "")
            if status == "ok":
                statuses = result.get("response", {}).get("data", {}).get("statuses", [])
                for s in statuses:
                    if "filled" in s:
                        f = s["filled"]
                        log(f"✅ Filled: {f.get('totalSz')} @ {f.get('avgPx')}")
                        return True
                    elif "resting" in s:
                        log(f"⚠️  Order resting (partial fill?): {s}")
                        return True
                    elif "error" in s:
                        log(f"❌ Order error: {s['error']}")
                        return False
            return "err" not in str(result).lower()
        else:
            log(f"HL close HTTP {r3.status_code}: {r3.text[:200]}")
            return False

    except Exception as e:
        log(f"HL close error: {e}")
        import traceback
        traceback.print_exc()
        return False

# ── Main loop ────────────────────────────────────────────────────────

def main():
    load_env()

    hl_address = os.environ.get("HL_USER_ADDRESS", "")
    hl_key = os.environ.get("HL_PRIVATE_KEY", "")
    lighter_idx = int(os.environ.get("LIGHTER_ACCOUNT_INDEX", "0"))
    coin = os.environ.get("HL_COIN", "HYPE")
    once = "--once" in sys.argv

    if not hl_address:
        log("ERROR: HL_USER_ADDRESS not set")
        sys.exit(1)

    # Start Lighter WS watcher
    lighter = LighterPositionWatcher(lighter_idx)
    lighter.start()

    log(f"Started | HL={hl_address[:10]}... | Lighter acct={lighter_idx}")
    log(f"Interval={CHECK_INTERVAL}s | Threshold={MISMATCH_THRESHOLD}")

    # Wait for first Lighter position
    for _ in range(30):
        if lighter.get_position() is not None:
            break
        time.sleep(1)
    else:
        log("WARNING: Could not get initial Lighter position, continuing anyway")

    prev_mismatch_net = None  # net exposure from previous mismatch

    while True:
        try:
            hl_pos = get_hl_position(hl_address, coin)
            lt_pos = lighter.get_position()

            if hl_pos is None:
                log("HL unavailable, skip")
            elif lt_pos is None:
                log(f"Lighter unavailable, HL={hl_pos:.4f}, skip")
            else:
                # Positions should be opposite: net = hl + lighter ≈ 0
                net = hl_pos + lt_pos

                if abs(net) <= MISMATCH_THRESHOLD:
                    # All good
                    if prev_mismatch_net is not None:
                        log(f"✅ Resolved. HL={hl_pos:.4f} LT={lt_pos:.4f} net={net:.4f}")
                        prev_mismatch_net = None
                    else:
                        log(f"OK HL={hl_pos:.4f} LT={lt_pos:.4f} net={net:.4f}")
                else:
                    log(f"⚠️  MISMATCH HL={hl_pos:.4f} LT={lt_pos:.4f} net={net:.4f}")

                    if prev_mismatch_net is not None:
                        # Same direction and similar magnitude?
                        if abs(net - prev_mismatch_net) < MISMATCH_THRESHOLD:
                            log(f"🚨 CONFIRMED NAKED x2: net={net:.4f} (prev={prev_mismatch_net:.4f})")

                            if hl_key:
                                # Close excess on HL
                                success = close_hl_position_simple(
                                    hl_address, hl_key, coin, net
                                )
                                if success:
                                    log("Auto-close sent, resetting monitor")
                                else:
                                    log("Auto-close FAILED, will retry next cycle")
                            else:
                                log("No HL_PRIVATE_KEY — cannot auto-close!")

                            prev_mismatch_net = None
                        else:
                            log(f"Mismatch changed ({prev_mismatch_net:.4f}→{net:.4f}), update")
                            prev_mismatch_net = net
                    else:
                        log(f"First mismatch, confirming in {CHECK_INTERVAL}s...")
                        prev_mismatch_net = net

        except Exception as e:
            log(f"Loop error: {e}")

        if once:
            break
        time.sleep(CHECK_INTERVAL)

    lighter.stop()


if __name__ == "__main__":
    main()

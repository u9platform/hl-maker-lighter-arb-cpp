#!/usr/bin/env python3
"""Thin bridge exposing real HL/Lighter actions for the C++ engine."""

from __future__ import annotations

import argparse
import asyncio
import os
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def emit(**kwargs: object) -> None:
    for key, value in kwargs.items():
        print(f"{key}={value}")


def _require_old_repo() -> Path:
    repo = os.environ.get("LIGHTER_HL_ARB_SOURCE")
    if repo:
        path = Path(repo)
    else:
        path = ROOT.parent / "lighter-hl-arb"
    if not path.exists():
        raise RuntimeError(
            "Set LIGHTER_HL_ARB_SOURCE to the old Python repo so the bridge can import real clients."
        )
    sys.path.insert(0, str(path))
    return path


async def hl_orderbook(args: argparse.Namespace) -> int:
    _require_old_repo()
    from src.hl_client import HLClient

    client = HLClient(api_url=os.environ.get("HL_API_URL"), private_key=os.environ.get("HL_PRIVATE_KEY"))
    try:
        ob = await client.get_orderbook(coin=args.coin, depth=5)
        emit(status="ok", bid=ob.best_bid, ask=ob.best_ask, quote_age_ms=int(client.quote_age_ms))
        return 0
    finally:
        await client.close()


async def hl_place_limit(args: argparse.Namespace) -> int:
    _require_old_repo()
    from src.hl_client import HLClient

    client = HLClient(
        api_url=os.environ.get("HL_API_URL"),
        private_key=os.environ.get("HL_PRIVATE_KEY"),
        account_address=os.environ.get("HL_ACCOUNT_ADDRESS"),
    )
    try:
        result = await client.place_limit_order(
            coin=args.coin,
            is_buy=args.side == "buy",
            price=args.price,
            size=args.size,
            post_only=args.post_only,
            dry_run=args.dry_run,
        )
        if "error" in result:
            emit(status="error", message=result["error"])
            return 1
        emit(status="ok", oid=result.get("oid", result.get("oid", "dry-run")), message="placed")
        return 0
    finally:
        await client.close()


async def hl_cancel(args: argparse.Namespace) -> int:
    _require_old_repo()
    from src.hl_client import HLClient

    client = HLClient(
        api_url=os.environ.get("HL_API_URL"),
        private_key=os.environ.get("HL_PRIVATE_KEY"),
        account_address=os.environ.get("HL_ACCOUNT_ADDRESS"),
    )
    try:
        oid = int(args.oid) if args.oid.isdigit() else args.oid
        result = await client.cancel_order(coin=args.coin, oid=oid, dry_run=args.dry_run)
        if "error" in result:
            emit(status="error", message=result["error"], oid=args.oid)
            return 1
        emit(status="ok", message="cancelled", oid=result.get("oid", args.oid))
        return 0
    finally:
        await client.close()


async def hl_reduce(args: argparse.Namespace) -> int:
    _require_old_repo()
    from src.hl_client import HLClient

    client = HLClient(
        api_url=os.environ.get("HL_API_URL"),
        private_key=os.environ.get("HL_PRIVATE_KEY"),
        account_address=os.environ.get("HL_ACCOUNT_ADDRESS"),
    )
    try:
        result = await client.reduce_position(
            coin=args.coin,
            size=args.size,
            is_buy=args.side == "buy",
            dry_run=args.dry_run,
        )
        if "error" in result:
            emit(status="error", message=result["error"])
            return 1
        emit(
            status="ok",
            message="reduced",
            filled_size=result.get("filled_size", 0.0),
            avg_fill_price=result.get("avg_fill_price", 0.0),
        )
        return 0
    finally:
        await client.close()


async def lighter_orderbook(args: argparse.Namespace) -> int:
    _require_old_repo()
    from src.lighter_client import LighterClient

    client = LighterClient(
        api_url=os.environ.get("LIGHTER_API_URL", "https://mainnet.zklighter.elliot.ai"),
        market_id=args.market_id,
        api_private_key=os.environ.get("LIGHTER_API_PRIVATE_KEY"),
        account_index=int(os.environ.get("LIGHTER_ACCOUNT_INDEX", "0")),
        api_key_index=int(os.environ.get("LIGHTER_API_KEY_INDEX", "0")),
    )
    try:
        ob = await client.get_orderbook(depth=5)
        quote_age_ms = int((time.time() - ob.timestamp) * 1000)
        emit(status="ok", bid=ob.best_bid, ask=ob.best_ask, quote_age_ms=quote_age_ms)
        return 0
    finally:
        close = getattr(client, "close", None)
        if close is not None:
            maybe = close()
            if asyncio.iscoroutine(maybe):
                await maybe


async def lighter_place_ioc(args: argparse.Namespace) -> int:
    _require_old_repo()
    from src.lighter_client import LighterClient

    client = LighterClient(
        api_url=os.environ.get("LIGHTER_API_URL", "https://mainnet.zklighter.elliot.ai"),
        market_id=int(os.environ.get("LIGHTER_MARKET_ID", "24")),
        api_private_key=os.environ.get("LIGHTER_API_PRIVATE_KEY"),
        account_index=int(os.environ.get("LIGHTER_ACCOUNT_INDEX", "0")),
        api_key_index=int(os.environ.get("LIGHTER_API_KEY_INDEX", "0")),
    )
    try:
        result = await client.place_ioc_order(
            is_ask=args.side == "sell",
            price=args.price,
            size=args.size,
            dry_run=args.dry_run,
        )
        if "error" in result:
            emit(status="error", message=result["error"])
            return 1
        emit(status="ok", message="ioc_sent", tx_hash=result.get("tx_hash", ""))
        return 0
    finally:
        close = getattr(client, "close", None)
        if close is not None:
            maybe = close()
            if asyncio.iscoroutine(maybe):
                await maybe


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="exchange", required=True)

    hl = sub.add_parser("hl")
    hl_sub = hl.add_subparsers(dest="action", required=True)

    hl_orderbook_parser = hl_sub.add_parser("orderbook")
    hl_orderbook_parser.add_argument("--coin", default="HYPE")
    hl_orderbook_parser.set_defaults(handler=hl_orderbook)

    hl_limit = hl_sub.add_parser("place-limit")
    hl_limit.add_argument("--coin", default="HYPE")
    hl_limit.add_argument("--side", choices=["buy", "sell"], required=True)
    hl_limit.add_argument("--price", type=float, required=True)
    hl_limit.add_argument("--size", type=float, required=True)
    hl_limit.add_argument("--post-only", action="store_true")
    hl_limit.add_argument("--dry-run", action="store_true")
    hl_limit.set_defaults(handler=hl_place_limit)

    hl_cancel_parser = hl_sub.add_parser("cancel")
    hl_cancel_parser.add_argument("--coin", default="HYPE")
    hl_cancel_parser.add_argument("--oid", required=True)
    hl_cancel_parser.add_argument("--dry-run", action="store_true")
    hl_cancel_parser.set_defaults(handler=hl_cancel)

    hl_reduce_parser = hl_sub.add_parser("reduce")
    hl_reduce_parser.add_argument("--coin", default="HYPE")
    hl_reduce_parser.add_argument("--side", choices=["buy", "sell"], required=True)
    hl_reduce_parser.add_argument("--size", type=float, required=True)
    hl_reduce_parser.add_argument("--dry-run", action="store_true")
    hl_reduce_parser.set_defaults(handler=hl_reduce)

    lighter = sub.add_parser("lighter")
    lighter_sub = lighter.add_subparsers(dest="action", required=True)

    lighter_orderbook_parser = lighter_sub.add_parser("orderbook")
    lighter_orderbook_parser.add_argument("--market-id", type=int, default=24)
    lighter_orderbook_parser.set_defaults(handler=lighter_orderbook)

    lighter_ioc = lighter_sub.add_parser("place-ioc")
    lighter_ioc.add_argument("--side", choices=["buy", "sell"], required=True)
    lighter_ioc.add_argument("--price", type=float, required=True)
    lighter_ioc.add_argument("--size", type=float, required=True)
    lighter_ioc.add_argument("--dry-run", action="store_true")
    lighter_ioc.set_defaults(handler=lighter_place_ioc)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        return asyncio.run(args.handler(args))
    except Exception as exc:
        emit(status="error", message=str(exc))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

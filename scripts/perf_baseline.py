#!/usr/bin/env python3
"""Measure current bridge and market-data latency baselines."""

from __future__ import annotations

import argparse
import statistics
import subprocess
import time
from pathlib import Path


def run_once(command: list[str]) -> float:
    start = time.perf_counter()
    subprocess.run(command, check=True, capture_output=True, text=True)
    return (time.perf_counter() - start) * 1000.0


def summarize(values: list[float]) -> tuple[float, float, float]:
    return statistics.mean(values), statistics.median(values), max(values)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=int, default=20)
    parser.add_argument(
        "--bridge",
        default=str(Path(__file__).resolve().with_name("exchange_bridge.py")),
    )
    parser.add_argument("--python-bin", default="python3")
    args = parser.parse_args()

    commands = {
        "hl_orderbook": [args.python_bin, args.bridge, "hl", "orderbook", "--coin", "HYPE"],
        "lighter_orderbook": [args.python_bin, args.bridge, "lighter", "orderbook", "--market-id", "24"],
    }

    for name, command in commands.items():
        values = [run_once(command) for _ in range(args.samples)]
        mean, p50, max_v = summarize(values)
        print(f"{name}: count={len(values)} mean_ms={mean:.2f} p50_ms={p50:.2f} max_ms={max_v:.2f}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

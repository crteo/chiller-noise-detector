#!/usr/bin/env python3
"""Estimate power-bank runtime from an inline USB meter observation."""

import argparse


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--usable-wh", type=float, default=60.0)
    parser.add_argument("--wh-used", type=float, required=True)
    parser.add_argument("--test-hours", type=float, required=True)
    args = parser.parse_args()
    if args.usable_wh <= 0 or args.wh_used <= 0 or args.test_hours <= 0:
        raise SystemExit("all values must be positive")

    watts = args.wh_used / args.test_hours
    daily_wh = watts * 24.0
    runtime_hours = args.usable_wh / watts
    print(f"Measured average power: {watts * 1000:.2f} mW")
    print(f"Measured daily energy: {daily_wh:.3f} Wh/day")
    print(
        f"Estimated runtime: {runtime_hours:.1f} hours "
        f"({runtime_hours / 24:.1f} days)"
    )


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Estimate four-AA runtime from measured average pack current."""

import argparse


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--capacity-mah", type=float, default=2500.0)
    parser.add_argument(
        "--average-current-ma",
        type=float,
        default=7.3,
        help="Measured at the AA holder, including LM2596 idle current",
    )
    parser.add_argument(
        "--usable-fraction",
        type=float,
        default=0.85,
        help="Capacity derating for cutoff, temperature, age and conversion loss",
    )
    args = parser.parse_args()
    if args.capacity_mah <= 0 or args.average_current_ma <= 0:
        raise SystemExit("capacity and current must be positive")
    if not 0 < args.usable_fraction <= 1:
        raise SystemExit("usable fraction must be between 0 and 1")

    usable_mah = args.capacity_mah * args.usable_fraction
    hours = usable_mah / args.average_current_ma
    print(f"Usable capacity: {usable_mah:.0f} mAh")
    print(f"Estimated runtime: {hours:.1f} hours ({hours / 24:.1f} days)")


if __name__ == "__main__":
    main()

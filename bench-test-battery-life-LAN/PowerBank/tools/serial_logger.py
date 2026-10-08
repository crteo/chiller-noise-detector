#!/usr/bin/env python3
"""Save CSV measurement rows emitted by the battery-bench master."""

import argparse
import csv
from datetime import datetime, timezone
from pathlib import Path

import serial

FIELDS = [
    "received_utc",
    "node_id",
    "power_source",
    "sequence",
    "rtc_unix",
    "a_weighted_dbfs",
    "estimated_dba",
    "peak_dbfs",
    "sample_count",
    "integration_ms",
    "fft_frames",
    "dominant_frequency_hz",
    "capture_ms",
    "awake_ms",
    "wifi_rssi_dbm",
]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("port", help="For example /dev/cu.usbmodem1101")
    parser.add_argument("output", type=Path)
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    new_file = not args.output.exists() or args.output.stat().st_size == 0
    if not new_file:
        with args.output.open("r", newline="", encoding="utf-8") as existing:
            header = next(csv.reader(existing), [])
        if header != FIELDS:
            parser.error(
                "existing CSV uses a different schema; choose a new output filename"
            )
    with serial.Serial(args.port, args.baud, timeout=1) as connection, args.output.open(
        "a", newline="", encoding="utf-8"
    ) as stream:
        writer = csv.writer(stream)
        if new_file:
            writer.writerow(FIELDS)
            stream.flush()
        while True:
            line = connection.readline().decode("utf-8", errors="replace").strip()
            if not line.startswith("CSV,"):
                continue
            values = line.split(",")[1:]
            received = datetime.now(timezone.utc).isoformat()
            writer.writerow([received, *values])
            stream.flush()
            print(line)


if __name__ == "__main__":
    main()

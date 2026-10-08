#!/usr/bin/env python3
"""Receive Stage 6 WAVs, verify them, store metadata, and sync master UTC."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import threading
import time
import urllib.error
import urllib.request
import wave
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


SAFE_TOKEN = re.compile(r"[^A-Za-z0-9_.-]+")
WRITE_LOCK = threading.Lock()
METADATA_FIELDS = [
    "capture_start_utc",
    "received_utc",
    "device_id",
    "role",
    "session_id",
    "capture_id",
    "filename",
    "wav_path",
    "bytes",
    "sha256",
    "sample_rate_hz",
    "bits_per_sample",
    "channels",
    "frame_count",
    "duration_seconds",
    "firmware_version",
    "wifi_rssi_dbm",
    "source_ip",
]


def safe_token(value: str, fallback: str) -> str:
    cleaned = SAFE_TOKEN.sub("_", value).strip("._")
    return cleaned or fallback


def utc_text(value: datetime) -> str:
    return value.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def inspect_wav(path: Path) -> dict[str, int | float]:
    with wave.open(str(path), "rb") as source:
        if source.getcomptype() != "NONE":
            raise ValueError("WAV must contain uncompressed PCM")
        channels = source.getnchannels()
        width = source.getsampwidth()
        rate = source.getframerate()
        frames = source.getnframes()
    if channels != 1 or width != 4 or rate != 32000 or frames <= 0:
        raise ValueError(
            f"Unexpected WAV geometry: channels={channels}, width={width}, "
            f"rate={rate}, frames={frames}"
        )
    return {
        "sample_rate_hz": rate,
        "bits_per_sample": width * 8,
        "channels": channels,
        "frame_count": frames,
        "duration_seconds": frames / rate,
    }


def append_metadata(output_dir: Path, metadata: dict[str, object]) -> None:
    csv_path = output_dir / "metadata.csv"
    jsonl_path = output_dir / "metadata.jsonl"
    with WRITE_LOCK:
        needs_header = not csv_path.exists() or csv_path.stat().st_size == 0
        with csv_path.open("a", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=METADATA_FIELDS)
            if needs_header:
                writer.writeheader()
            writer.writerow(metadata)
            stream.flush()
            os.fsync(stream.fileno())
        with jsonl_path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(metadata, sort_keys=True) + "\n")
            stream.flush()
            os.fsync(stream.fileno())


class CaptureHandler(BaseHTTPRequestHandler):
    server_version = "Stage6AcousticCollector/1.0"

    def do_POST(self) -> None:  # noqa: N802
        if self.path != "/upload":
            self.send_error(404, "Use POST /upload")
            return
        if self.headers.get("X-API-Key") != self.server.api_key:
            self.send_error(401, "Invalid API key")
            return
        try:
            content_length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.send_error(400, "Invalid Content-Length")
            return
        if content_length < 44 or content_length > self.server.max_upload_bytes:
            self.send_error(413, "Unexpected upload size")
            return

        device_id = safe_token(self.headers.get("X-Device-Id", "unknown"), "unknown")
        role = safe_token(self.headers.get("X-Role", "unknown"), "unknown")
        filename = safe_token(self.headers.get("X-Filename", "capture.wav"), "capture.wav")
        if not filename.lower().endswith(".wav"):
            self.send_error(400, "Filename must end with .wav")
            return
        try:
            capture_time = datetime.strptime(
                self.headers.get("X-Capture-Start-Utc", ""), "%Y-%m-%dT%H:%M:%SZ"
            ).replace(tzinfo=timezone.utc)
            session_id = int(self.headers.get("X-Session-Id", "0"))
            capture_id = int(self.headers.get("X-Capture-Id", "0"))
        except ValueError:
            self.send_error(400, "Invalid capture timestamp or identifier")
            return
        if session_id <= 0 or capture_id <= 0:
            self.send_error(400, "Session and capture IDs must be positive")
            return

        destination_dir = (
            self.server.output_dir
            / device_id
            / capture_time.strftime("%Y")
            / capture_time.strftime("%m")
            / capture_time.strftime("%d")
        )
        destination_dir.mkdir(parents=True, exist_ok=True)
        destination = destination_dir / filename
        temporary = destination.with_suffix(destination.suffix + ".part")
        if destination.exists():
            self.send_error(409, "Capture already exists")
            return

        digest = hashlib.sha256()
        remaining = content_length
        try:
            with temporary.open("xb") as output:
                while remaining:
                    chunk = self.rfile.read(min(64 * 1024, remaining))
                    if not chunk:
                        raise ConnectionError("Client disconnected during upload")
                    output.write(chunk)
                    digest.update(chunk)
                    remaining -= len(chunk)
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary, destination)
            properties = inspect_wav(destination)
        except Exception as exc:
            temporary.unlink(missing_ok=True)
            destination.unlink(missing_ok=True)
            self.log_error("Upload rejected: %s", exc)
            self.send_error(422, "Incomplete or invalid WAV")
            return

        metadata = {
            "capture_start_utc": utc_text(capture_time),
            "received_utc": utc_text(datetime.now(timezone.utc)),
            "device_id": device_id,
            "role": role,
            "session_id": session_id,
            "capture_id": capture_id,
            "filename": filename,
            "wav_path": str(destination.resolve()),
            "bytes": content_length,
            "sha256": digest.hexdigest(),
            **properties,
            "firmware_version": self.headers.get("X-Firmware-Version", ""),
            "wifi_rssi_dbm": self.headers.get("X-Wifi-Rssi-Dbm", ""),
            "source_ip": self.client_address[0],
        }
        append_metadata(self.server.output_dir, metadata)
        body = json.dumps({"saved": str(destination), "sha256": digest.hexdigest()}).encode()
        self.send_response(201)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        self.log_message("Saved %s (%d frames)", destination, properties["frame_count"])

    def log_message(self, format: str, *args: object) -> None:
        stamp = utc_text(datetime.now(timezone.utc))
        print(f"{stamp} {self.client_address[0]} {format % args}", flush=True)


def synchronize_master(master_url: str, api_key: str) -> bool:
    payload = json.dumps({"unix_ms": time.time_ns() // 1_000_000}).encode()
    request = urllib.request.Request(
        f"{master_url.rstrip('/')}/api/v1/time",
        data=payload,
        method="POST",
        headers={"Content-Type": "application/json", "X-API-Key": api_key},
    )
    try:
        with urllib.request.urlopen(request, timeout=3) as response:
            return response.status == 200
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        print(f"Master clock sync failed: {exc}", flush=True)
        return False


def clock_sync_loop(
    master_url: str, api_key: str, interval_seconds: int, stop: threading.Event
) -> None:
    while not stop.is_set():
        if synchronize_master(master_url, api_key):
            print(f"Synchronized master UTC at {utc_text(datetime.now(timezone.utc))}", flush=True)
        stop.wait(interval_seconds)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--output", type=Path, default=Path("stage-6/captures"))
    parser.add_argument("--max-upload-mb", type=int, default=4)
    parser.add_argument("--master-url", default="http://192.168.4.1")
    parser.add_argument("--sync-seconds", type=int, default=60)
    parser.add_argument("--api-key", required=True)
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer((args.host, args.port), CaptureHandler)
    server.output_dir = args.output.resolve()
    server.max_upload_bytes = args.max_upload_mb * 1024 * 1024
    server.api_key = args.api_key

    stop = threading.Event()
    sync_thread = threading.Thread(
        target=clock_sync_loop,
        args=(args.master_url, args.api_key, args.sync_seconds, stop),
        daemon=True,
    )
    sync_thread.start()
    print(f"Listening on http://{args.host}:{args.port}/upload", flush=True)
    print(f"Saving WAV and capture metadata under {server.output_dir}", flush=True)
    print("Per-sample PCM CSV export is intentionally disabled", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping collector", flush=True)
    finally:
        stop.set()
        server.server_close()
        sync_thread.join(timeout=2)


if __name__ == "__main__":
    main()

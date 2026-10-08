#!/usr/bin/env python3
"""Stage 7 local-first WAV collector and optional Firebase uploader."""

from __future__ import annotations

import argparse
import hashlib
import json
import logging
import os
import re
import secrets
import sqlite3
import struct
import threading
import time
import urllib.request
import wave
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

import numpy as np
LOG = logging.getLogger("stage7.collector")
SAFE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$")
REQUIRED_HEADERS = (
    "X-Device-Id", "X-Role", "X-Session-Id", "X-Capture-Id",
    "X-Capture-Start-Utc", "X-Sample-Rate", "X-Bits-Per-Sample", "X-Channels",
)


def load_config(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        cfg = json.load(handle)
    for section in ("pilot", "server", "master", "local_storage", "analysis", "firebase"):
        if section not in cfg:
            raise ValueError(f"Missing config section: {section}")
    key = str(cfg["server"].get("shared_key", ""))
    if len(key) < 20 or key.startswith("replace-"):
        raise ValueError("server.shared_key must be a real random value of at least 20 characters")
    if not SAFE_ID.fullmatch(str(cfg["pilot"].get("asset_id", ""))):
        raise ValueError("pilot.asset_id may contain only letters, numbers, dot, underscore and hyphen")
    return cfg


def resolve_path(config_path: Path, value: str) -> Path:
    path = Path(value).expanduser()
    return path if path.is_absolute() else (config_path.parent / path).resolve()


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def validate_id(value: str, field: str) -> str:
    if not SAFE_ID.fullmatch(value):
        raise ValueError(f"Invalid {field}")
    return value


class Database:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.path = path
        with self.connect() as db:
            db.executescript("""
                PRAGMA journal_mode=WAL;
                CREATE TABLE IF NOT EXISTS captures (
                    capture_key TEXT PRIMARY KEY,
                    asset_id TEXT NOT NULL,
                    node_id TEXT NOT NULL,
                    role TEXT NOT NULL,
                    session_id TEXT NOT NULL,
                    capture_id INTEGER NOT NULL,
                    capture_start_utc TEXT NOT NULL,
                    received_utc TEXT NOT NULL,
                    raw_path TEXT NOT NULL,
                    preview_path TEXT NOT NULL,
                    spectrum_path TEXT NOT NULL,
                    sha256 TEXT NOT NULL,
                    metadata_json TEXT NOT NULL,
                    analysis_json TEXT NOT NULL,
                    cloud_state TEXT NOT NULL DEFAULT 'pending',
                    cloud_attempts INTEGER NOT NULL DEFAULT 0,
                    cloud_error TEXT,
                    cloud_updated_utc TEXT,
                    UNIQUE(node_id, session_id, capture_id)
                );
            """)

    def connect(self) -> sqlite3.Connection:
        db = sqlite3.connect(self.path, timeout=30)
        db.row_factory = sqlite3.Row
        return db

    def insert(self, record: dict[str, Any]) -> bool:
        columns = ",".join(record)
        placeholders = ",".join("?" for _ in record)
        try:
            with self.connect() as db:
                db.execute(f"INSERT INTO captures ({columns}) VALUES ({placeholders})", tuple(record.values()))
            return True
        except sqlite3.IntegrityError:
            return False

    def pending(self, limit: int = 10) -> list[sqlite3.Row]:
        with self.connect() as db:
            return db.execute(
                "SELECT * FROM captures WHERE cloud_state IN ('pending','retry') ORDER BY received_utc LIMIT ?",
                (limit,),
            ).fetchall()

    def cloud_result(self, key: str, success: bool, error: str | None = None) -> None:
        with self.connect() as db:
            db.execute(
                "UPDATE captures SET cloud_state=?, cloud_attempts=cloud_attempts+1, cloud_error=?, cloud_updated_utc=? WHERE capture_key=?",
                ("uploaded" if success else "retry", error, utc_now(), key),
            )


def read_pcm32_wav(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path), "rb") as wav:
        if wav.getnchannels() != 1 or wav.getsampwidth() != 4 or wav.getcomptype() != "NONE":
            raise ValueError("Expected mono, uncompressed, 32-bit PCM WAV")
        sample_rate = wav.getframerate()
        samples = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i4").astype(np.float64)
    if not samples.size:
        raise ValueError("WAV contains no samples")
    return samples / 2147483648.0, sample_rate


def make_preview(raw_path: Path, preview_path: Path) -> None:
    with wave.open(str(raw_path), "rb") as source:
        frames = source.readframes(source.getnframes())
        sample_rate = source.getframerate()
    pcm32 = np.frombuffer(frames, dtype="<i4")
    pcm16 = np.clip(np.rint(pcm32.astype(np.float64) / 65536.0), -32768, 32767).astype("<i2")
    preview_path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(preview_path), "wb") as target:
        target.setnchannels(1)
        target.setsampwidth(2)
        target.setframerate(sample_rate)
        target.writeframes(pcm16.tobytes())


def analyse(raw_path: Path, cfg: dict[str, Any]) -> dict[str, Any]:
    samples, sample_rate = read_pcm32_wav(raw_path)
    samples = samples - float(np.mean(samples))
    window = np.hanning(samples.size)
    spectrum = np.fft.rfft(samples * window)
    power = (np.abs(spectrum) ** 2) / max(float(np.sum(window**2)), 1e-20)
    frequencies = np.fft.rfftfreq(samples.size, 1.0 / sample_rate)
    low = float(cfg["minimum_hz"])
    high = min(float(cfg["maximum_hz"]), sample_rate / 2)
    mask = (frequencies >= low) & (frequencies <= high)
    selected_f = frequencies[mask]
    selected_p = power[mask]
    points = max(32, min(int(cfg["spectrum_points"]), 512))
    edges = np.linspace(low, high, points + 1)
    indices = np.clip(np.digitize(selected_f, edges) - 1, 0, points - 1)
    binned = np.zeros(points)
    counts = np.zeros(points)
    np.add.at(binned, indices, selected_p)
    np.add.at(counts, indices, 1)
    binned /= np.maximum(counts, 1)
    frequency_axis = ((edges[:-1] + edges[1:]) / 2).round(2)
    dbfs_axis = (10 * np.log10(np.maximum(binned, 1e-20))).round(2)
    rms = float(np.sqrt(np.mean(samples**2)))
    peak = float(np.max(np.abs(samples)))
    total = max(float(np.sum(selected_p)), 1e-20)
    centroid = float(np.sum(selected_f * selected_p) / total)
    probabilities = selected_p / total
    entropy = float(-np.sum(probabilities * np.log2(np.maximum(probabilities, 1e-20))) / np.log2(max(probabilities.size, 2)))
    flatness = float(np.exp(np.mean(np.log(np.maximum(selected_p, 1e-20)))) / max(np.mean(selected_p), 1e-20))
    return {
        "sample_rate_hz": sample_rate,
        "sample_count": int(samples.size),
        "duration_seconds": round(samples.size / sample_rate, 3),
        "rms_dbfs": round(20 * np.log10(max(rms, 1e-10)), 3),
        "peak_dbfs": round(20 * np.log10(max(peak, 1e-10)), 3),
        "crest_factor": round(peak / max(rms, 1e-10), 4),
        "spectral_centroid_hz": round(centroid, 2),
        "spectral_entropy": round(entropy, 5),
        "spectral_flatness": round(flatness, 6),
        "spectrum_hz": frequency_axis.tolist(),
        "spectrum_dbfs": dbfs_axis.tolist(),
    }


class Cloud:
    def __init__(self, cfg: dict[str, Any], config_path: Path):
        self.cfg = cfg
        self.enabled = bool(cfg["enabled"])
        self.db = self.bucket = self.firestore = None
        if not self.enabled:
            return
        try:
            import firebase_admin
            from firebase_admin import credentials, firestore, storage
        except ImportError as exc:
            raise RuntimeError("Firebase is enabled but firebase-admin is not installed") from exc
        credential_path = resolve_path(config_path, cfg["service_account_file"])
        if not credential_path.is_file():
            raise FileNotFoundError(f"Firebase service account not found: {credential_path}")
        firebase_admin.initialize_app(credentials.Certificate(str(credential_path)), {"storageBucket": cfg["storage_bucket"]})
        self.firestore = firestore
        self.db = firestore.client()
        self.bucket = storage.bucket()

    def upload(self, row: sqlite3.Row) -> None:
        if not self.enabled:
            return
        metadata = json.loads(row["metadata_json"])
        analysis = json.loads(row["analysis_json"])
        asset_id = row["asset_id"]
        capture_doc = f'{row["session_id"]}-{row["capture_id"]}'
        node_id = row["node_id"]
        base = f"assets/{asset_id}/{row['session_id']}/{row['capture_id']}/{node_id}"
        preview_cloud = raw_cloud = None
        if self.cfg.get("upload_preview_audio", True):
            preview_cloud = f"{base}/preview.wav"
            self.bucket.blob(preview_cloud).upload_from_filename(row["preview_path"], content_type="audio/wav")
        if self.cfg.get("upload_raw_audio", False):
            raw_cloud = f"{base}/raw-s32.wav"
            self.bucket.blob(raw_cloud).upload_from_filename(row["raw_path"], content_type="audio/wav")
        node = {
            **metadata,
            **analysis,
            "sha256": row["sha256"],
            "preview_storage_path": preview_cloud,
            "raw_storage_path": raw_cloud,
            "uploaded_at": self.firestore.SERVER_TIMESTAMP,
        }
        reference = self.db.collection("assets").document(asset_id).collection("captures").document(capture_doc)
        reference.set({
            "asset_id": asset_id,
            "session_id": row["session_id"],
            "capture_id": row["capture_id"],
            "capture_start_utc": row["capture_start_utc"],
            "received_utc": row["received_utc"],
            "updated_at": self.firestore.SERVER_TIMESTAMP,
        }, merge=True)
        # merge=True recursively preserves a capture's other node map while
        # avoiding dotted field-path parsing for IDs containing hyphens.
        reference.set({"nodes": {node_id: node}}, merge=True)

    def status(self, asset_id: str, status: dict[str, Any]) -> None:
        if self.enabled:
            self.db.collection("assets").document(asset_id).set(
                {"current": status, "current_updated_at": self.firestore.SERVER_TIMESTAMP}, merge=True
            )


class Runtime:
    def __init__(self, cfg: dict[str, Any], config_path: Path):
        self.cfg = cfg
        self.config_path = config_path
        self.asset_id = cfg["pilot"]["asset_id"]
        self.root = resolve_path(config_path, cfg["local_storage"]["root"])
        self.root.mkdir(parents=True, exist_ok=True)
        self.database = Database(resolve_path(config_path, cfg["local_storage"]["database"]))
        self.cloud = Cloud(cfg["firebase"], config_path)
        self.stop = threading.Event()

    def save_upload(self, headers: Any, body: bytes) -> tuple[bool, str]:
        metadata = {name: headers[name] for name in REQUIRED_HEADERS}
        node_id = validate_id(metadata["X-Device-Id"], "device id")
        role = metadata["X-Role"]
        if role not in ("near", "reference"):
            raise ValueError("Role must be near or reference")
        session_id = validate_id(metadata["X-Session-Id"], "session id")
        capture_id = int(metadata["X-Capture-Id"])
        if capture_id <= 0:
            raise ValueError("Capture id must be positive")
        stamp = metadata["X-Capture-Start-Utc"]
        datetime.fromisoformat(stamp.replace("Z", "+00:00"))
        day = stamp[:10]
        capture_key = f"{session_id}-{capture_id}-{node_id}"
        directory = self.root / "captures" / day / session_id / f"capture-{capture_id}"
        directory.mkdir(parents=True, exist_ok=True)
        raw_path = directory / f"{node_id}-raw-s32.wav"
        preview_path = directory / f"{node_id}-preview-s16.wav"
        spectrum_path = directory / f"{node_id}-analysis.json"
        temporary = directory / f".{node_id}-{secrets.token_hex(4)}.tmp"
        temporary.write_bytes(body)
        try:
            samples, sample_rate = read_pcm32_wav(temporary)
            if sample_rate != int(metadata["X-Sample-Rate"]):
                raise ValueError("WAV sample rate does not match header")
            if int(metadata["X-Bits-Per-Sample"]) != 32 or int(metadata["X-Channels"]) != 1:
                raise ValueError("Only mono 32-bit PCM uploads are accepted")
            os.replace(temporary, raw_path)
            make_preview(raw_path, preview_path)
            analysis = analyse(raw_path, self.cfg["analysis"])
            spectrum_path.write_text(json.dumps(analysis, indent=2), encoding="utf-8")
            received = utc_now()
            clean_metadata = {
                "node_id": node_id,
                "role": role,
                "firmware_version": headers.get("X-Firmware-Version", "unknown"),
                "capture_start_utc": stamp,
                "wifi_rssi_dbm": int(headers.get("X-Wifi-Rssi-Dbm", "0")),
            }
            optional_numbers = {
                "sensor_broadband_dbfs": "X-Broadband-Dbfs",
                "estimated_dbz": "X-Estimated-Dbz",
                "estimated_dba": "X-Estimated-Dba",
                "sensor_crest_factor": "X-Crest-Factor",
                "kurtosis": "X-Kurtosis",
                "zero_crossing_rate": "X-Zero-Crossing-Rate",
                "clipped_fraction": "X-Clipped-Fraction",
            }
            for field, header in optional_numbers.items():
                if headers.get(header) is not None:
                    clean_metadata[field] = float(headers[header])
            record = {
                "capture_key": capture_key,
                "asset_id": self.asset_id,
                "node_id": node_id,
                "role": role,
                "session_id": session_id,
                "capture_id": capture_id,
                "capture_start_utc": stamp,
                "received_utc": received,
                "raw_path": str(raw_path),
                "preview_path": str(preview_path),
                "spectrum_path": str(spectrum_path),
                "sha256": hashlib.sha256(body).hexdigest(),
                "metadata_json": json.dumps(clean_metadata),
                "analysis_json": json.dumps(analysis),
            }
            inserted = self.database.insert(record)
            if not inserted:
                return False, capture_key
            LOG.info("Stored %s (%d samples, %.2f s)", capture_key, samples.size, samples.size / sample_rate)
            return True, capture_key
        finally:
            temporary.unlink(missing_ok=True)

    def cloud_loop(self) -> None:
        retry = max(5, int(self.cfg["firebase"].get("retry_seconds", 60)))
        while not self.stop.wait(1):
            if not self.cloud.enabled:
                self.stop.wait(retry)
                continue
            pending = self.database.pending()
            if not pending:
                self.stop.wait(5)
                continue
            for row in pending:
                try:
                    self.cloud.upload(row)
                    self.database.cloud_result(row["capture_key"], True)
                    LOG.info("Cloud upload complete: %s", row["capture_key"])
                except Exception as exc:  # queue is deliberately persistent
                    self.database.cloud_result(row["capture_key"], False, str(exc)[:500])
                    LOG.warning("Cloud upload deferred for %s: %s", row["capture_key"], exc)
                    self.stop.wait(retry)
                    break

    def master_loop(self) -> None:
        master = self.cfg["master"]
        interval = max(5, int(master["poll_seconds"]))
        timeout = float(master["request_timeout_seconds"])
        base = master["base_url"].rstrip("/")
        while not self.stop.is_set():
            try:
                if master.get("sync_clock", True):
                    body = json.dumps({"unix_ms": int(time.time() * 1000)}).encode()
                    request = urllib.request.Request(
                        f"{base}/api/v1/time", data=body, method="POST",
                        headers={"X-API-Key": self.cfg["server"]["shared_key"], "Content-Type": "application/json"},
                    )
                    with urllib.request.urlopen(request, timeout=timeout) as response:
                        response.read()
                with urllib.request.urlopen(f"{base}/api/v1/status", timeout=timeout) as response:
                    status = json.loads(response.read())
                self.cloud.status(self.asset_id, status)
            except Exception as exc:
                LOG.warning("Master status/sync unavailable: %s", exc)
            self.stop.wait(interval)


def make_handler(runtime: Runtime):
    class Handler(BaseHTTPRequestHandler):
        server_version = "Stage7Collector/1.0"

        def send_json(self, status: int, payload: dict[str, Any]) -> None:
            encoded = json.dumps(payload).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(encoded)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(encoded)

        def do_GET(self) -> None:
            if self.path == "/health":
                self.send_json(200, {"ok": True, "asset_id": runtime.asset_id, "firebase": runtime.cloud.enabled})
            else:
                self.send_json(404, {"error": "not_found"})

        def do_POST(self) -> None:
            if self.path != "/upload":
                self.send_json(404, {"error": "not_found"})
                return
            if not secrets.compare_digest(self.headers.get("X-API-Key", ""), runtime.cfg["server"]["shared_key"]):
                self.send_json(401, {"error": "unauthorized"})
                return
            missing = [name for name in REQUIRED_HEADERS if not self.headers.get(name)]
            if missing:
                self.send_json(400, {"error": "missing_headers", "headers": missing})
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                maximum = int(runtime.cfg["server"]["max_upload_bytes"])
                if length < 45 or length > maximum:
                    raise ValueError("Invalid Content-Length")
                body = self.rfile.read(length)
                if len(body) != length:
                    raise ValueError("Incomplete request body")
                inserted, key = runtime.save_upload(self.headers, body)
                self.send_json(201 if inserted else 200, {"stored": inserted, "duplicate": not inserted, "capture_key": key})
            except (ValueError, wave.Error, struct.error) as exc:
                self.send_json(422, {"error": "invalid_upload", "detail": str(exc)})
            except Exception:
                LOG.exception("Upload failed")
                self.send_json(500, {"error": "internal_error"})

        def log_message(self, fmt: str, *args: Any) -> None:
            LOG.info("%s - %s", self.client_address[0], fmt % args)

    return Handler


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=Path(__file__).with_name("config.json"))
    parser.add_argument("--check", action="store_true", help="validate configuration and exit")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    config_path = args.config.resolve()
    cfg = load_config(config_path)
    if args.check:
        print(f"Configuration valid: {config_path}")
        return
    runtime = Runtime(cfg, config_path)
    cloud_thread = threading.Thread(target=runtime.cloud_loop, name="firebase-uploader", daemon=True)
    master_thread = threading.Thread(target=runtime.master_loop, name="master-monitor", daemon=True)
    cloud_thread.start()
    master_thread.start()
    server = ThreadingHTTPServer((cfg["server"]["bind_host"], int(cfg["server"]["port"])), make_handler(runtime))
    LOG.info("Collector listening on http://%s:%s; local root=%s", *server.server_address, runtime.root)
    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        LOG.info("Stopping collector")
    finally:
        runtime.stop.set()
        server.server_close()


if __name__ == "__main__":
    main()

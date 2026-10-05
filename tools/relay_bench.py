#!/usr/bin/env python3
"""Supervised, app-only relay diagnostic with mandatory production restoration.

Never invoke against connected hardware without explicit operator permission.
This module's tests inject all USB, flashing, and timing operations.
"""
import argparse
import hashlib
import importlib.metadata
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

from device_setup import usb_exchange

APP_OFFSET = "0x30000"
APP_SIZE = 0x300000
BENCH_ID = "TSL_USB_RELAY_BENCH"


def image(path, digest):
    path = Path(path).resolve(strict=True)
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ValueError("Expected SHA-256 must be lowercase hex")
    data = path.read_bytes()
    if not 32 <= len(data) <= APP_SIZE or data[0] != 0xE9:
        raise ValueError("Invalid ESP application image")
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError("Image does not match the reviewed SHA-256")
    return path


def stage_image(path, digest, destination):
    # Flashing rereads files. Freeze reviewed bytes so a concurrent build cannot
    # replace the restore image between preflight verification and restoration.
    destination = Path(destination)
    destination.write_bytes(Path(path).read_bytes())
    image(destination, digest)
    return destination


def inhibited(state, diagnostic, expected_mac):
    values = {"ready": True, "disabled": True, "commissioned": False,
              "dry_run": True, "commanded_on": False, "fault": False}
    if any(state.get(key) is not value for key, value in values.items()):
        raise RuntimeError("Production controller must be inhibited")
    if (diagnostic.get("station_mac") != expected_mac.lower() or
            diagnostic.get("profile_integrity") is not True or
            diagnostic.get("token_state") != "usable" or
            diagnostic.get("provision_pending") is not False):
        raise RuntimeError("Unexpected board identity or storage state")


class Hardware:
    def __init__(self, port, expected_mac):
        self.port = port
        self.expected_mac = expected_mac

    def production(self):
        state = usb_exchange(self.port, {"op": "status"}, timeout=5)
        diagnostic = usb_exchange(self.port, {"op": "diagnostics"}, timeout=5)
        hello = usb_exchange(self.port, {"op": "hello"}, timeout=5)
        if hello.get("board") != "ESP32-S3-Relay-1CH":
            raise RuntimeError("Unexpected board")
        inhibited(state, diagnostic, self.expected_mac)
        return state

    def esptool(self, operation, path):
        # write_flash verifies each block's hash before reporting success.
        subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3",
                        "--port", self.port, "--baud", "460800", "--before",
                        "default_reset", "--after", "hard_reset", operation,
                        APP_OFFSET, str(path)], check=True, timeout=120)

    def bench(self, command):
        import serial
        # Opening a serial handle must not assert reset/download control lines.
        connection = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=1)
        connection.dtr = False
        connection.rts = False
        connection.port = self.port
        try:
            connection.open()
            connection.reset_input_buffer()
            connection.write((command + "\n").encode("ascii"))
            connection.flush()
            deadline = time.monotonic() + 2
            line = bytearray()
            while time.monotonic() < deadline:
                byte = connection.read(1)
                if not byte:
                    continue
                if byte == b"\n":
                    try:
                        result = json.loads(line)
                    except (ValueError, UnicodeError):
                        line.clear()
                        continue
                    if isinstance(result, dict):
                        return result
                    raise RuntimeError("Invalid bench response")
                line.extend(byte)
                if len(line) > 1024:
                    raise RuntimeError("Oversized bench response")
            raise RuntimeError("Bench acknowledgement missing; ON is not retried")
        finally:
            connection.close()


def bench_state(state, *, nonce=None):
    if (state.get("bench") != BENCH_ID or state.get("protocol") != 1 or
            state.get("fault") is not False or
            type(state.get("commanded_on")) is not bool or
            type(state.get("used")) is not bool or
            not re.fullmatch(r"[0-9a-f]{32}", state.get("nonce", ""))):
        raise RuntimeError("Unexpected bench identity/state")
    if nonce is not None and state["nonce"] != nonce:
        raise RuntimeError("Bench rebooted; pulse must not be retried")
    return state


def run(hw, bench_path, production_path, sleep=time.sleep, emit=print):
    hw.production()
    # Prove the supplied restore image matches installed bytes before replacing
    # anything. A mismatch aborts without attempting any flash write.
    hw.esptool("verify_flash", production_path)
    emit("Production image matches installed bytes. Installing bench application only.")
    try:
        hw.esptool("write_flash", bench_path)
        sleep(3)
        initial = bench_state(hw.bench("STATUS"))
        if initial["commanded_on"] or initial["used"]:
            raise RuntimeError("Bench is not initially unused and OFF")
        dwell = initial.get("dwell_remaining_ms")
        if type(dwell) is not int or not 0 <= dwell <= 30000:
            raise RuntimeError("Invalid OFF dwell")
        emit("Keep watching isolated COM-NO: waiting for the 30-second OFF dwell.")
        sleep(dwell / 1000 + 0.1)
        ready = bench_state(hw.bench("STATUS"), nonce=initial["nonce"])
        if ready["commanded_on"] or ready["used"] or ready.get("dwell_remaining_ms") != 0:
            raise RuntimeError("Bench is not ready")
        emit("One two-second relay pulse begins in five seconds. Watch the meter now.")
        sleep(5)
        # This is the sole START call; a missing acknowledgement never retries it.
        ack = bench_state(hw.bench("START " + initial["nonce"]), nonce=initial["nonce"])
        if ack.get("ok") is not True or ack.get("commanded_on") is not True:
            raise RuntimeError("Pulse not acknowledged; outcome requires observation")
        sleep(2.3)
        final = bench_state(hw.bench("STATUS"), nonce=initial["nonce"])
        if final["commanded_on"] or not final["used"]:
            raise RuntimeError("Expected completed one-shot OFF state")
        emit("Diagnostic reports OFF after the pulse; physical contact result is operator-observed.")
    finally:
        # Also runs for partial flash, malformed replies, lost START ack or Ctrl-C.
        # A hard power loss cannot execute this block: see recovery instructions.
        try:
            hw.bench("OFF")
        except Exception:
            pass
        emit("Restoring production application; no NVS, bootloader or partition erase.")
        hw.esptool("write_flash", production_path)
        hw.esptool("verify_flash", production_path)
        sleep(5)
        restored = hw.production()
        emit(json.dumps({"production_restored": True, "status": restored}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--expected-mac", required=True)
    parser.add_argument("--bench-image", required=True, type=Path)
    parser.add_argument("--bench-sha256", required=True)
    parser.add_argument("--production-image", required=True, type=Path)
    parser.add_argument("--production-sha256", required=True)
    parser.add_argument("--authorize-isolated-usb-relay-test", action="store_true")
    args = parser.parse_args()
    if not args.authorize_isolated_usb_relay_test:
        parser.error("Explicit permission for temporary firmware and a relay pulse is required")
    if not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", args.expected_mac):
        parser.error("Expected MAC must be colon-separated hexadecimal")
    staging = None
    try:
        if importlib.metadata.version("esptool") != "4.12.0":
            raise RuntimeError("Use pinned esptool 4.12.0")
        bench_path = image(args.bench_image, args.bench_sha256)
        production_path = image(args.production_image, args.production_sha256)
        if bench_path == production_path or args.bench_sha256 == args.production_sha256:
            raise ValueError("Bench and production images must be distinct")
        staging = Path(tempfile.mkdtemp(prefix="tsl-relay-bench-"))
        frozen_bench = stage_image(bench_path, args.bench_sha256, staging / "bench.bin")
        frozen_production = stage_image(production_path, args.production_sha256,
                                        staging / "production.bin")
        run(Hardware(args.port, args.expected_mac), frozen_bench, frozen_production,
            emit=lambda message: print(message, flush=True))
        shutil.rmtree(staging)
    except (Exception, KeyboardInterrupt) as exc:
        print(json.dumps({"bench_error_type": type(exc).__name__,
                          "retained_image_directory": str(staging) if staging else None,
                          "expected_production_sha256": args.production_sha256,
                          "instruction": "Keep mains disconnected. Verify restoration before further use; see docs/relay_bench.md."}))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

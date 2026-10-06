"""Synthetic host workflow; never imports or opens a real serial transport."""
import copy
import contextlib
import hashlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import relay_bench as bench


class FakeHardware:
    def __init__(self):
        self.calls = []
        self.fail = None
        self.state = {"bench": bench.BENCH_ID, "protocol": 1, "nonce": "a" * 32,
                      "dwell_remaining_ms": 10000, "commanded_on": False,
                      "used": False, "fault": False}
        self.status_count = 0
        self.bad_final = False
        self.reboot = False

    def operation(self, name, data=None):
        self.calls.append((name, data))
        if self.fail == (name, data):
            self.fail = None
            raise RuntimeError("Synthetic failure")

    def production(self):
        self.operation("production")
        return {"disabled": True, "commanded_on": False}

    def esptool(self, operation, path):
        self.operation(operation, path)

    def bench(self, command):
        self.operation("bench", command)
        if command.startswith("START "):
            self.state["used"] = True
            return {**self.state, "ok": True, "commanded_on": True}
        if command == "OFF":
            return {"ok": True, "commanded_on": False}
        self.status_count += 1
        state = copy.deepcopy(self.state)
        if self.status_count > 1:
            state["dwell_remaining_ms"] = 0
        if self.reboot and self.status_count > 1:
            state["nonce"] = "b" * 32
        if self.bad_final and self.status_count > 2:
            state["commanded_on"] = True
        return state


class BenchWorkflowTests(unittest.TestCase):
    def run_fake(self, hw):
        bench.run(hw, "bench.bin", "production.bin", sleep=lambda _: None,
                  emit=lambda _: None)

    def assert_restored(self, hw):
        self.assertEqual(hw.calls[-3:], [("write_flash", "production.bin"),
                                        ("verify_flash", "production.bin"),
                                        ("production", None)])

    def test_success_issues_one_start_and_restores(self):
        hw = FakeHardware()
        self.run_fake(hw)
        self.assertEqual(hw.calls.count(("bench", "START " + "a" * 32)), 1)
        self.assert_restored(hw)

    def test_preflight_rejection_never_flashes(self):
        for fault in [("production", None), ("verify_flash", "production.bin")]:
            with self.subTest(fault=fault):
                hw = FakeHardware()
                hw.fail = fault
                with self.assertRaises(RuntimeError):
                    self.run_fake(hw)
                self.assertFalse(any(op == "write_flash" for op, _ in hw.calls))

    def test_partial_bench_write_still_restores(self):
        hw = FakeHardware()
        hw.fail = ("write_flash", "bench.bin")
        with self.assertRaises(RuntimeError):
            self.run_fake(hw)
        self.assert_restored(hw)

    def test_missing_start_ack_never_retries_and_restores(self):
        hw = FakeHardware()
        hw.fail = ("bench", "START " + "a" * 32)
        with self.assertRaises(RuntimeError):
            self.run_fake(hw)
        self.assertEqual(hw.calls.count(("bench", "START " + "a" * 32)), 1)
        self.assert_restored(hw)

    def test_reset_during_dwell_cannot_start(self):
        hw = FakeHardware()
        hw.reboot = True
        with self.assertRaises(RuntimeError):
            self.run_fake(hw)
        self.assertFalse(any(op == "bench" and val.startswith("START ") for op, val in hw.calls))
        self.assert_restored(hw)

    def test_missing_off_confirmation_still_restores(self):
        hw = FakeHardware()
        hw.bad_final = True
        with self.assertRaises(RuntimeError):
            self.run_fake(hw)
        self.assert_restored(hw)

    def test_off_transport_failure_does_not_skip_restore(self):
        hw = FakeHardware()
        hw.fail = ("bench", "OFF")
        self.run_fake(hw)
        self.assert_restored(hw)

    def test_restore_failure_is_not_success(self):
        hw = FakeHardware()
        hw.fail = ("write_flash", "production.bin")
        with self.assertRaises(RuntimeError):
            self.run_fake(hw)
        self.assertNotEqual(hw.calls[-1], ("production", None))

    def test_image_requires_matching_hash_and_application_header(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.bin"
            payload = b"\xe9" + bytes(99)
            path.write_bytes(payload)
            self.assertEqual(bench.image(path, hashlib.sha256(payload).hexdigest()), path)
            with self.assertRaises(ValueError):
                bench.image(path, "0" * 64)
            path.write_bytes(bytes(100))
            with self.assertRaises(ValueError):
                bench.image(path, hashlib.sha256(bytes(100)).hexdigest())

    def test_production_check_rejects_real_output_or_wrong_identity(self):
        state = {"ready": True, "disabled": True, "commissioned": False,
                 "dry_run": True, "commanded_on": False, "fault": False}
        diag = {"station_mac": "00:00:00:00:00:01", "profile_integrity": True,
                "token_state": "usable", "provision_pending": False}
        bench.inhibited(state, diag, diag["station_mac"])
        for key in state:
            changed = dict(state)
            changed[key] = not changed[key]
            with self.subTest(key=key), self.assertRaises(RuntimeError):
                bench.inhibited(changed, diag, diag["station_mac"])
        with self.assertRaises(RuntimeError):
            bench.inhibited(state, diag, "00:00:00:00:00:02")

    def test_staged_restore_survives_original_build_replacement(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "build.bin"
            destination = Path(directory) / "restore.bin"
            original = b"\xe9" + bytes(99)
            source.write_bytes(original)
            frozen = bench.stage_image(source, hashlib.sha256(original).hexdigest(), destination)
            source.write_bytes(b"\xe9" + b"x" * 99)
            self.assertEqual(frozen.read_bytes(), original)
            with self.assertRaises(ValueError):
                bench.stage_image(source, hashlib.sha256(original).hexdigest(), destination)

    def test_cli_keeps_reviewed_images_on_failure_cleans_only_after_success(self):
        for failure in (RuntimeError("synthetic-sensitive-detail"),
                        bench.BenchError("Unexpected board"), None):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                production = root / "production.bin"
                diagnostic = root / "bench.bin"
                production.write_bytes(b"\xe9" + bytes(99))
                diagnostic.write_bytes(b"\xe9" + b"b" * 99)
                stage = root / "staging"
                stage.mkdir()
                digest = hashlib.sha256(production.read_bytes()).hexdigest()
                argv = ["relay_bench.py", "--port", "FAKE", "--expected-mac", "00:00:00:00:00:01",
                        "--production-image", str(production), "--production-sha256", digest,
                        "--bench-image", str(diagnostic), "--bench-sha256",
                        hashlib.sha256(diagnostic.read_bytes()).hexdigest(),
                        "--authorize-isolated-usb-relay-test"]
                with patch.object(sys, "argv", argv), \
                        patch.object(bench.importlib.metadata, "version", return_value="4.12.0"), \
                        patch.object(bench.tempfile, "mkdtemp", return_value=str(stage)), \
                        patch.object(bench, "run", side_effect=failure), \
                        contextlib.redirect_stdout(io.StringIO()) as output:
                    self.assertEqual(bench.main(), 1 if failure else 0)
                self.assertEqual(stage.exists(), bool(failure))
                self.assertNotIn("synthetic-sensitive-detail", output.getvalue())
                if isinstance(failure, bench.BenchError):
                    self.assertIn('"bench_error_reason": "Unexpected board"', output.getvalue())
                if failure:
                    self.assertEqual((stage / "production.bin").read_bytes(), production.read_bytes())


if __name__ == "__main__":
    unittest.main()

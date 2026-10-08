"""Synthetic local-controller recorder tests; no hardware or Tesla requests."""
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import observe_controller as observer


def status(uptime=100, generation=1, on=True):
    return {"mode": "AUTO", "uptime_s": uptime, "generation": generation,
            "gpio_command": "ON commanded" if on else "OFF commanded",
            "auto_home": on, "fault": False, "vehicle": "online",
            "reason": "auto_home_lease" if on else "no_auto_authorization",
            "lease_s": 500 if on else 0, "position_basis": "vehicle_report",
            "error": "none", "vin": "synthetic-private-vin", "access_token": "synthetic-secret",
            "settings": {"vin": "synthetic-private-vin", "home_lat": 1.25, "home_lon": 2.75,
                         "lease_s": 600, "poll_s": 540}}


class Reply:
    def __init__(self, raw, headers=None):
        self.stream = io.BytesIO(raw)
        self.headers = headers or {}

    def read1(self, size):
        return self.stream.read(size)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        pass


class ObserverTests(unittest.TestCase):
    def client(self):
        with mock.patch.object(observer.ssl, "create_default_context"):
            return observer.LocalClient("https://10.23.45.67", Path("synthetic.pem"), "synthetic-password")

    def test_origin_rejects_external_hosts_credentials_and_paths(self):
        self.assertEqual(observer.origin_url("https://10.23.45.67/"), "https://10.23.45.67")
        for value in ("http://10.23.45.67", "https://example.com", "https://8.8.8.8",
                      "https://user:secret@10.23.45.67", "https://10.23.45.67/api/action",
                      "https://10.23.45.67?token=secret", "https://10.23.45.67:444"):
            with self.subTest(value=value), self.assertRaises(observer.ObserverError):
                observer.origin_url(value)

    def test_secrets_and_coordinates_are_excluded_from_status_and_events(self):
        safe = observer.safe_status(status())
        self.assertEqual(safe["policy"], {"lease_s": 600, "poll_s": 540})
        self.assertNotIn("synthetic-private-vin", json.dumps(safe))
        self.assertNotIn("synthetic-secret", json.dumps(safe))
        event = {"uptime_s": 1, "reason": "auto_home_lease", "commanded_on": True, "token": "secret"}
        self.assertNotIn("secret", json.dumps(observer.safe_events({"events": [event]})))
        for invalid in (dict(status(), mode="secret"), dict(status(), uptime_s=True),
                        dict(status(), distance_m=float("inf")), dict(status(), reserved_month=[1, -1, 3])):
            with self.assertRaises(observer.ObserverError):
                observer.safe_status(invalid)
        with self.assertRaises(observer.ObserverError):
            observer.safe_events({"events": [event] * 17})

    def test_password_file_reads_only_unique_LOCAL_entry(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "env.txt"
            path.write_text("TESLA_CLIENT_SECRET=ignored-secret\nLOCAL=synthetic-password\n")
            self.assertEqual(observer.password_from_file(path), "synthetic-password")
            path.write_text("LOCAL=synthetic-password\nLOCAL=another-password\n")
            with self.assertRaises(observer.ObserverError):
                observer.password_from_file(path)

    def test_request_routes_cannot_trigger_fleet_output_or_settings(self):
        client = self.client()
        for path, payload in (("/api/action", {"action": "off"}), ("/api/action", {"action": "check"}),
                              ("/api/action", {"action": "auto"}), ("/api/settings", {}),
                              ("/api/1/vehicles", None), ("/api/status", {"action": "off"})):
            with self.subTest(path=path, payload=payload), self.assertRaises(observer.ObserverError):
                client.request(path, payload)

    def test_bounded_reply_and_no_token_in_errors(self):
        client = self.client()
        for raw in (b"x" * (observer.MAX_REPLY + 1), b'{"access_token":"secret",',
                    b'{"duplicate":1,"duplicate":2}', b'{"value":NaN}'):
            client.opener.open = mock.Mock(return_value=Reply(raw))
            with self.assertRaises(observer.ObserverError) as captured:
                client.request("/api/status")
            self.assertNotIn("secret", str(captured.exception))
        client.opener.open = mock.Mock(return_value=Reply(b"{}", {"Content-Encoding": "gzip"}))
        with self.assertRaisesRegex(observer.ObserverError, "unsupported_encoding"):
            client.request("/api/status")

    def test_expired_session_reauthenticates_once_and_captures_events(self):
        client = self.client()
        client.authenticated = True
        client.login = mock.Mock()
        client.request = mock.Mock(side_effect=[observer.ObserverError("local_http_401"), status(), {"events": []}])
        self.assertTrue(client.snapshot()["status"]["auto_home"])
        client.login.assert_called_once()
        client.request = mock.Mock(side_effect=[observer.ObserverError("local_http_401"), observer.ObserverError("local_http_401")])
        with self.assertRaises(observer.ObserverError):
            client.snapshot()

    def test_wrong_login_stops_instead_of_retrying(self):
        client = self.client()
        client.request = mock.Mock(side_effect=[{"version": 2, "iterations": 100000, "salt": "00" * 16},
                                               observer.ObserverError("local_http_401")])
        with self.assertRaises(observer.ObserverError) as captured:
            client.login()
        self.assertTrue(captured.exception.fatal)

    def test_reboot_and_disconnect_are_recorded_without_stopping_monitor(self):
        stop = threading.Event()
        client = mock.Mock()
        client.snapshot.side_effect = [{"status": observer.safe_status(status(200, 1))},
                                       observer.ObserverError("local_transport_or_tls"),
                                       {"status": observer.safe_status(status(30, 2, False))}]
        client.close.return_value = "logged_out"
        waits = []

        def wait(delay):
            waits.append(delay)
            if len(waits) == 3:
                stop.set()

        with tempfile.TemporaryDirectory() as temporary, mock.patch.object(stop, "wait", side_effect=wait), mock.patch("builtins.print"):
            output = Path(temporary) / "private" / "trace.jsonl"
            self.assertEqual(observer.record(client, output, 1000, 60, stop), 0)
            rows = [json.loads(line) for line in output.read_text().splitlines()]
            self.assertEqual(rows[2]["observer_error"], "local_transport_or_tls")
            self.assertTrue(rows[3]["uptime_decreased"])
            self.assertTrue(rows[3]["generation_changed"])
            self.assertEqual(rows[-1]["cleanup"], "logged_out")
            client.close.assert_called_once()
            if os.name != "nt":
                self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            with self.assertRaises(FileExistsError):
                observer.record(client, output, 1000, 60, stop)

    def test_log_cap_stops_and_closes_session(self):
        client = mock.Mock()
        client.snapshot.return_value = {"status": observer.safe_status(status())}
        client.close.return_value = "logged_out"
        with tempfile.TemporaryDirectory() as temporary, mock.patch.object(observer, "MAX_LOG", 256), mock.patch("builtins.print"):
            output = Path(temporary) / "trace.jsonl"
            self.assertEqual(observer.record(client, output, 1000, 60, threading.Event()), 1)
            self.assertLessEqual(output.stat().st_size, 256)
            client.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()

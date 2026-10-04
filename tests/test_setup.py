"""Offline only: fake Tesla/USB, temporary synthetic keys, no live HTTP requests."""
import datetime as dt
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import tesla_setup as tesla
import device_setup as device
import onboard
from cryptography import x509
from cryptography.hazmat.primitives import serialization

class OAuthTests(unittest.TestCase):
    def test_exact_https_redirect_state_and_duplicates(self):
        redirect = "https://example.com/callback.html"
        self.assertEqual(tesla.callback_code(redirect + "?code=synthetic&state=abc", redirect, "abc"), "synthetic")
        for value in ["http://localhost/callback", "https://example.com/path#fragment", "https://user@example.com/x"]:
            with self.assertRaises(tesla.SetupError): tesla.validate_redirect(value)
        for callback in ["https://evil.example/callback.html?code=x&state=abc",
                         redirect + "?code=x&state=wrong", redirect + "?code=x&state=abc&state=abc",
                         redirect + "/?code=x&state=abc", redirect + "?error=denied&state=abc"]:
            with self.assertRaises(tesla.SetupError): tesla.callback_code(callback, redirect, "abc")

    def test_only_read_scopes_no_loopback(self):
        url = tesla.authorization_url("synthetic", "https://example.com/callback.html", "state")
        scopes = tesla.urllib.parse.parse_qs(tesla.urllib.parse.urlsplit(url).query)["scope"][0].split()
        self.assertEqual(set(scopes), {"openid", "offline_access", "vehicle_device_data", "vehicle_location"})
        self.assertFalse(any("cmd" in s for s in scopes))

    def test_official_endpoint_allowlist_and_redirect_rejection(self):
        for url in ["http://fleet-api.prd.na.vn.cloud.tesla.com/api/1/vehicles",
                    "https://fleet-api.prd.na.vn.cloud.tesla.com.evil.example/", "https://user:pass@fleet-api.prd.na.vn.cloud.tesla.com/"]:
            with self.assertRaises(tesla.SetupError): tesla.request(url)
        with self.assertRaises(tesla.SetupError):
            tesla.NoRedirect().redirect_request(None, None, 302, "", {}, "https://evil.example")

    def test_lossless_vehicle_identifiers_and_explicit_selection(self):
        parsed = tesla.decode_json(b'{"id":9007199254740993,"vin":"5YJ3E1EA7KF000001"}')
        self.assertEqual(parsed["id"], 9007199254740993)
        self.assertIs(tesla.select_vehicle([parsed], parsed["vin"]), parsed)
        with self.assertRaises(tesla.SetupError): tesla.select_vehicle([parsed], "")
        with self.assertRaises(tesla.SetupError): tesla.select_vehicle([parsed, parsed], parsed["vin"])

    def test_malformed_oversized_and_nested_json(self):
        for body in [b'{"x":1,"x":2}', b'{"x":NaN}', b'[' * 14 + b'0' + b']' * 14,
                     b' ' * (tesla.MAX_BODY + 1), b'broken']:
            with self.assertRaises(tesla.SetupError): tesla.decode_json(body)

    def test_numeric_overflow_is_rejected_at_every_json_depth(self):
        for body in [b'1e9999', b'-1e9999', b'{"x":1e9999}', b'[{"x":[-1e9999]}]']:
            with self.subTest(body=body), self.assertRaisesRegex(tesla.SetupError, "Non-finite"):
                tesla.decode_json(body)
        self.assertEqual(tesla.decode_json(b'{"x":1.79e9,"id":9007199254740993}'),
                         {"x": 1790000000.0, "id": 9007199254740993})

    def test_http_errors_redact_token_url_body_and_headers(self):
        class Opener:
            def open(self, request, timeout):
                raise tesla.urllib.error.HTTPError("https://secret.invalid?token=DO_NOT_PRINT", 403,
                    "DO_NOT_PRINT", {}, None)
        with patch.object(tesla.urllib.request, "build_opener", return_value=Opener()):
            with self.assertRaises(tesla.SetupError) as raised:
                tesla.request(tesla.REGIONS["NA"] + "/api/1/vehicles", token="DO_NOT_PRINT")
        self.assertNotIn("DO_NOT_PRINT", str(raised.exception))
        self.assertIn("403", str(raised.exception))

    def test_paginated_selection_never_follows_remote_url(self):
        rows = [{"response": [{"vin": "5YJ3E1EA7KF000001"}], "pagination": {"next": "https://evil.example"}},
                {"response": [{"vin": "5YJ3E1EA7KF000002"}], "pagination": {"next": None}}]
        with patch.object(tesla, "request", side_effect=rows) as fake:
            result = tesla.vehicles("synthetic", "NA")
        self.assertEqual(len(result), 2)
        self.assertTrue(fake.call_args_list[1].args[0].startswith(tesla.REGIONS["NA"]))

class ScanTests(unittest.TestCase):
    def test_usb_scan_displays_untrusted_ssids_without_terminal_escapes(self):
        ssid = b'synthetic"\\\n\x1b[31m\xff'
        response = {"ok": True, "truncated": False, "networks": [
            {"ssid_hex": ssid.hex(), "rssi": -61, "channel": 6, "authmode": 3}]}
        output = io.StringIO()
        with patch.object(onboard, "usb_exchange", return_value=response) as exchange, \
             patch.object(onboard, "hidden") as hidden, patch("sys.stdout", output):
            onboard.usb(SimpleNamespace(command="wifi_scan", port="synthetic"))
        exchange.assert_called_once_with("synthetic", {"op": "wifi_scan"})
        hidden.assert_not_called()
        self.assertNotIn("\x1b", output.getvalue())
        parsed = json.loads(output.getvalue())
        self.assertEqual(parsed["networks"][0]["ssid"], ssid.decode("utf-8", errors="backslashreplace"))
        self.assertEqual(parsed["networks"][0]["ssid_hex"], ssid.hex())

    def test_unavailable_scan_does_not_retry_or_request_password(self):
        output = io.StringIO()
        with patch.object(onboard, "usb_exchange", return_value={"ok": False, "error": "scan_unavailable"}) as exchange, \
             patch.object(onboard, "hidden") as hidden, patch("sys.stdout", output):
            with self.assertRaisesRegex(tesla.SetupError, "scan is unavailable"):
                onboard.usb(SimpleNamespace(command="wifi_scan", port="synthetic"))
        self.assertEqual(exchange.call_count, 1)
        hidden.assert_not_called()
        self.assertEqual(output.getvalue(), "")

class UsbCommandTests(unittest.TestCase):
    def test_mutating_commands_capture_generation_before_secret_prompt(self):
        for command in ("auto", "timed_on", "arm", "enable_output", "wifi_update"):
            args = SimpleNamespace(command=command, port="SYNTHETIC", seconds=3600)
            responses = [{"generation": 42}, {"ok": True, "committed": True}]
            confirmations = {"arm": "USB_BENCH_POLARITY_AND_STARTUP_VERIFIED",
                             "enable_output": "ALLOW_PHYSICAL_RELAY_AFTER_BENCH_CHECK",
                             "timed_on": "TIMED_ON", "wifi_update": "USB_WIFI_RECOVERY_KEEP_OUTPUT_OFF"}
            with (self.subTest(command=command),
                  patch.object(onboard, "usb_exchange", side_effect=responses) as exchange,
                  patch("builtins.input", return_value=confirmations.get(command, "")),
                  patch("sys.stdout", io.StringIO())):
                def hidden(_):
                    self.assertEqual(exchange.call_count, 1)
                    return "synthetic-password"
                with patch.object(onboard, "hidden", side_effect=hidden):
                    onboard.usb(args)
            self.assertEqual(exchange.call_args_list[0].args[1], {"op": "status"})
            self.assertEqual(exchange.call_args_list[1].args[1]["generation"], 42)

    def test_invalid_generation_stops_before_credentials_or_action(self):
        for generation in (None, True, 0, -1, 4294967296, 1.0, "42"):
            with (self.subTest(generation=generation),
                  patch.object(onboard, "usb_exchange", return_value={"generation": generation}) as exchange,
                  patch.object(onboard, "hidden") as hidden):
                with self.assertRaisesRegex(tesla.SetupError, "valid generation"):
                    onboard.usb(SimpleNamespace(command="auto", port="SYNTHETIC"))
            exchange.assert_called_once_with("SYNTHETIC", {"op": "status"})
            hidden.assert_not_called()

    def test_provisioning_rejects_old_protocol_before_credentials_or_consent(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "setup.json").write_text("{}")
            with (patch("builtins.input", return_value="PROVISION"),
                  patch.object(onboard, "usb_exchange", return_value={
                      "protocol": 1, "board": "ESP32-S3-Relay-1CH"}),
                  patch.object(onboard, "hidden") as hidden,
                  patch.object(onboard, "consent") as consent,
                  patch("sys.stdout", io.StringIO())):
                with self.assertRaisesRegex(tesla.SetupError, "protocol/board mismatch"):
                    onboard.run(root, "SYNTHETIC")
            hidden.assert_not_called()
            consent.assert_not_called()

    def test_rejected_or_missing_off_ack_fails_cli_without_printing_device_text(self):
        cases = [
            ({"ok": False, "error": "authentication_required"}, "USB authentication failed"),
            ({"ok": False, "error": "rejected"}, "USB command rejected"),
            ({"ok": False, "error": "synthetic-secret"}, "not acknowledged"),
            ({"ok": "true", "error": ["synthetic-secret"]}, "not acknowledged"),
            ({}, "not acknowledged"),
        ]
        for response, message in cases:
            response = dict(response, detail="synthetic-secret")
            with (self.subTest(response=response),
                  patch.object(sys, "argv", ["onboard.py", "usb", "--port", "SYNTHETIC", "off"]),
                  patch.object(onboard, "hidden", return_value="synthetic-admin-password"),
                  patch.object(onboard, "usb_exchange", return_value=response) as exchange,
                  patch("sys.stdout", new_callable=io.StringIO) as output,
                  patch("sys.stderr", new_callable=io.StringIO) as errors):
                self.assertEqual(onboard.main(), 1)
            exchange.assert_called_once()
            self.assertIn(message, errors.getvalue())
            self.assertNotIn("synthetic-secret", errors.getvalue() + output.getvalue())
            self.assertEqual(output.getvalue(), "")

    def test_read_only_metadata_without_ok_remains_successful(self):
        cases = [
            ("hello", {"protocol": 2, "board": "ESP32-S3-Relay-1CH"}),
            ("status", {"ready": True, "disabled": True, "commanded_on": False, "generation": 42}),
            ("diagnostics", {"token_state": "usable", "provision_stage": "idle"}),
        ]
        for command, response in cases:
            with (self.subTest(command=command),
                  patch.object(sys, "argv", ["onboard.py", "usb", "--port", "SYNTHETIC", command]),
                  patch.object(onboard, "hidden") as hidden,
                  patch.object(onboard, "usb_exchange", return_value=response),
                  patch("sys.stdout", new_callable=io.StringIO) as output):
                self.assertEqual(onboard.main(), 0)
            hidden.assert_not_called()
            self.assertEqual(json.loads(output.getvalue()), response)

    def test_acknowledged_off_succeeds(self):
        with (patch.object(sys, "argv", ["onboard.py", "usb", "--port", "SYNTHETIC", "off"]),
              patch.object(onboard, "hidden", return_value="synthetic-admin-password"),
              patch.object(onboard, "usb_exchange", return_value={"ok": True}),
              patch("sys.stdout", new_callable=io.StringIO) as output):
            self.assertEqual(onboard.main(), 0)
        self.assertTrue(json.loads(output.getvalue())["ok"])

class WifiRecoveryTests(unittest.TestCase):
    def test_usb_recovery_uses_hidden_prompts_and_preserves_other_secrets(self):
        output = io.StringIO()
        args = SimpleNamespace(command="wifi_update", port="SYNTHETIC_PORT")
        with patch.object(onboard, "hidden", side_effect=["synthetic-admin-password", "synthetic-ssid", "synthetic-wifi-password"]), \
             patch("builtins.input", return_value="USB_WIFI_RECOVERY_KEEP_OUTPUT_OFF"), \
             patch.object(onboard, "usb_exchange", side_effect=[{"generation": 42}, {"ok": True, "committed": True}]) as exchange, \
             patch("sys.stdout", output):
            onboard.usb(args)
        self.assertEqual(exchange.call_count, 2)
        request = exchange.call_args.args[1]
        self.assertEqual(request["op"], "wifi_update")
        self.assertEqual(request["wifi_ssid"], "synthetic-ssid")
        self.assertEqual(request["wifi_password"], "synthetic-wifi-password")
        self.assertNotIn("refresh_token", request)
        self.assertNotIn("client_id", request)
        self.assertNotIn("synthetic-admin-password", output.getvalue())
        self.assertNotIn("synthetic-wifi-password", output.getvalue())

    def test_invalid_wifi_recovery_does_not_write(self):
        args = SimpleNamespace(command="wifi_update", port="SYNTHETIC_PORT")
        with patch.object(onboard, "hidden", side_effect=["synthetic-admin-password", "synthetic-ssid", "short"]), \
             patch("builtins.input", return_value="USB_WIFI_RECOVERY_KEEP_OUTPUT_OFF"), \
             patch.object(onboard, "usb_exchange", return_value={"generation": 42}) as exchange, \
             patch("sys.stdout", io.StringIO()):
            with self.assertRaises(tesla.SetupError):
                onboard.usb(args)
        exchange.assert_called_once_with("SYNTHETIC_PORT", {"op": "status"})

class FilesTests(unittest.TestCase):
    def test_generated_public_tree_contains_no_private_keys(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "provisioning"
            device.prepare_files(root, "example.com", "contactor.example.test")
            for p in (root / "public").rglob("*"):
                if p.is_file(): self.assertNotIn(b"PRIVATE KEY", p.read_bytes())
            page = (root / "public" / "callback.html").read_text()
            self.assertNotIn("<script", page)
            self.assertIn("no-referrer", page)
            cert = x509.load_pem_x509_certificate((root / "device-cert.pem").read_bytes())
            san = cert.extensions.get_extension_for_class(x509.SubjectAlternativeName).value
            self.assertEqual(san.get_values_for_type(x509.DNSName), ["contactor.example.test"])
            key = serialization.load_pem_private_key((root / "private" / "device-key.pem").read_bytes(), None)
            self.assertEqual(key.public_key().public_numbers(), cert.public_key().public_numbers())
            if os.name != "nt":
                self.assertEqual((root / "private" / "device-key.pem").stat().st_mode & 0o777, 0o600)
            with self.assertRaises(FileExistsError): device.prepare_files(root, "example.com", "contactor.example.test")

    def test_ip_certificate_san(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "provisioning"
            device.prepare_files(root, "example.com", "192.0.2.1")
            cert = x509.load_pem_x509_certificate((root / "device-cert.pem").read_bytes())
            self.assertEqual(str(cert.extensions.get_extension_for_class(x509.SubjectAlternativeName).value[0].value), "192.0.2.1")

    def test_fake_usb_handoff_no_automatic_retry(self):
        import serial
        instances = []
        class Port:
            def __init__(self, **kwargs):
                self.data = bytearray(b'{"ok":true,"committed":true}\n')
                self.writes = []
                instances.append(self)
            def open(self): pass
            def reset_input_buffer(self): pass
            def write(self, b): self.writes.append(b)
            def flush(self): pass
            def read(self, n):
                b = self.data[:n]; del self.data[:n]; return b
            def close(self): self.closed = True
        with patch.object(serial, "Serial", Port):
            result = device.usb_exchange("SYNTHETIC_PORT", {"op": "provision", "refresh_token": "synthetic"})
        self.assertTrue(result["committed"])
        self.assertEqual(len(instances[0].writes), 1)
        self.assertTrue(instances[0].closed)
        self.assertFalse(instances[0].dtr)
        self.assertFalse(instances[0].rts)

if __name__ == "__main__":
    unittest.main()

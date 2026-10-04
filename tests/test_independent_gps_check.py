"""Synthetic tests for the one-shot, laptop-side diagnostic (no Tesla requests)."""
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import independent_gps_check as check
from tesla_setup import SetupError

VIN = "5YJ3E1EA7KF000001"  # Synthetic identifier, never a real vehicle.


class IndependentGpsCheckTests(unittest.TestCase):
    def test_secret_file_requires_explicit_key_and_never_uses_unkeyed_password(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "env.txt"
            path.write_text("local-admin-password\n")
            with self.assertRaisesRegex(SetupError, "TESLA_CLIENT_SECRET"):
                check.client_secret_from_file(path)
            path.write_text("local-admin-password\nTESLA_CLIENT_SECRET=synthetic-secret\n")
            self.assertEqual(check.client_secret_from_file(path), "synthetic-secret")
            path.write_text("TESLA_CLIENT_SECRET=one\nTESLA_CLIENT_SECRET=two\n")
            with self.assertRaisesRegex(SetupError, "TESLA_CLIENT_SECRET"):
                check.client_secret_from_file(path)

    def test_original_negative_number_survives_independent_json_parse(self):
        raw = (b'{"response":{"vin":"' + VIN.encode() + b'","drive_state":{'
               b'"latitude":1,"longitude":2,"gps_as_of":-123456789,'
               b'"timestamp":1800000000123},"api_version":95}}')
        result = check.inspect_location(raw, VIN)
        self.assertEqual(result["raw_gps_as_of"], "-123456789")
        self.assertEqual(result["decoded_gps_as_of"], -123456789)
        self.assertEqual(result["drive_state_timestamp"], 1800000000123)

    def test_wrong_identity_duplicate_or_missing_source_is_rejected(self):
        response = {"response": {"vin": VIN, "drive_state": {
            "latitude": 1, "longitude": 2, "gps_as_of": 1790000000}}}
        raw = json.dumps(response).encode()
        with self.assertRaises(SetupError):
            check.inspect_location(raw, "7SAYGDEF4SF000001")
        with self.assertRaises(SetupError):
            check.inspect_location(raw.replace(b'"gps_as_of": 1790000000', b'"gps_as_of": null'), VIN)
        with self.assertRaises(SetupError):
            check.inspect_location(raw.replace(b'"gps_as_of": 1790000000',
                                               b'"gps_as_of": 1790000000, "gps_as_of": 1790000001'), VIN)

    def test_one_shot_online_capture_writes_only_redacted_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "private").mkdir()
            (root / "setup.json").write_text(json.dumps({
                "client_id": "synthetic-client", "region": "NA",
                "redirect": "https://example.com/callback"}), encoding="utf-8")
            response = b'{"response":{"vin":"' + VIN.encode() + b'","state":"online"}}'
            location = (b'{"response":{"vin":"' + VIN.encode() + b'","drive_state":{'
                        b'"latitude":1,"longitude":2,"gps_as_of":-123456789}}}')
            calls = []

            def fake_get(url, access):
                calls.append(url)
                self.assertEqual(access, "synthetic-access")
                return 200, response if len(calls) == 1 else location, {"date": "synthetic", "x_txid": "synthetic"}

            with (mock.patch.object(sys, "argv", ["independent_gps_check.py", "--directory", str(root)]),
                  mock.patch("builtins.input", side_effect=[VIN, "CHECK"]),
                  mock.patch.object(check.getpass, "getpass", side_effect=[
                      "synthetic-secret", "https://example.com/callback?state=synthetic-state&code=synthetic-code"]),
                  mock.patch.object(check.secrets, "token_urlsafe", side_effect=["synthetic-state", "synthetic-nonce"]),
                  mock.patch.object(check, "request", return_value={
                      "access_token": "synthetic-access", "refresh_token": "synthetic-unused",
                      "scope": "openid vehicle_device_data vehicle_location"}),
                  mock.patch.object(check, "fleet_get", side_effect=fake_get),
                  mock.patch("builtins.print") as printed):
                check.main()

            self.assertEqual(len(calls), 2)
            self.assertTrue(calls[1].endswith("/vehicle_data?endpoints=location_data"))
            report = json.loads(next((root / "private").glob("gps-independent-*.json")).read_text())
            self.assertEqual(report["raw_gps_as_of"], "-123456789")
            self.assertNotIn("vin", report)
            self.assertNotIn("latitude", report)
            self.assertNotIn("refresh_token", report)
            self.assertNotIn("access_token", report)
            output = " ".join(" ".join(map(str, args)) for args, _ in
                              (call for call in printed.call_args_list))
            self.assertNotIn("synthetic-secret", output)
            self.assertNotIn("synthetic-access", output)
            self.assertNotIn("offline_access", output)

    def test_asleep_vehicle_does_not_request_location_or_save_a_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "private").mkdir()
            (root / "setup.json").write_text(json.dumps({
                "client_id": "synthetic-client", "region": "NA",
                "redirect": "https://example.com/callback"}), encoding="utf-8")
            calls = []

            def fake_get(url, access):
                calls.append(url)
                return 200, json.dumps({"response": {"vin": VIN, "state": "asleep"}}).encode(), {}

            with (mock.patch.object(sys, "argv", ["independent_gps_check.py", "--directory", str(root)]),
                  mock.patch("builtins.input", side_effect=[VIN, "CHECK"]),
                  mock.patch.object(check.getpass, "getpass", side_effect=[
                      "synthetic-secret", "https://example.com/callback?state=synthetic-state&code=synthetic-code"]),
                  mock.patch.object(check.secrets, "token_urlsafe", side_effect=["synthetic-state", "synthetic-nonce"]),
                  mock.patch.object(check, "request", return_value={"access_token": "synthetic-access"}),
                  mock.patch.object(check, "fleet_get", side_effect=fake_get),
                  mock.patch("builtins.print")):
                check.main()

            self.assertEqual(len(calls), 1)
            self.assertEqual(list((root / "private").iterdir()), [])


if __name__ == "__main__":
    unittest.main()

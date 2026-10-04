"""One-shot, laptop-side Fleet REST capture; never refreshes or provisions tokens.

Requires fresh, interactive read-only consent. The response body is inspected in
memory; only redacted metadata is written to a private local file.
"""
from __future__ import annotations

import argparse
import datetime as dt
import getpass
import json
import re
import secrets
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from device_setup import write_new
from tesla_setup import (MAX_BODY, REGIONS, TOKEN_URL, AUTHORIZE_URL, NoRedirect,
                         SetupError, callback_code, decode_json, request, valid_vin,
                         validate_redirect)

READ_SCOPES = "openid vehicle_device_data vehicle_location"
GPS_NUMBER = re.compile(rb'"gps_as_of"\s*:\s*(-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)')
BEARER = re.compile(r"[A-Za-z0-9._~+/=-]{1,4096}\Z")


def inspect_location(raw: bytes, vin: str) -> dict:
    """Compare the original JSON number text with an independent Python parse."""
    document = decode_json(raw)
    if not isinstance(document, dict) or not isinstance(document.get("response"), dict):
        raise SetupError("Unexpected Fleet response envelope.")
    response = document["response"]
    if response.get("vin") != vin or not isinstance(response.get("drive_state"), dict):
        raise SetupError("Fleet response VIN or drive_state mismatch.")
    drive = response["drive_state"]
    if not all(isinstance(drive.get(k), (float, int)) and not isinstance(drive[k], bool)
               for k in ("latitude", "longitude")):
        raise SetupError("Fleet response lacks numeric coordinates.")
    if not -90 <= drive["latitude"] <= 90 or not -180 <= drive["longitude"] <= 180:
        raise SetupError("Fleet response coordinates are out of range.")
    matches = GPS_NUMBER.findall(raw)
    if len(matches) != 1:
        raise SetupError("Expected exactly one numeric gps_as_of lexeme.")
    gps = drive.get("gps_as_of")
    if isinstance(gps, bool) or not isinstance(gps, (float, int)):
        raise SetupError("Fleet gps_as_of is not numeric.")
    text = matches[0].decode("ascii")
    if json.loads(text) != gps:
        raise SetupError("Raw gps_as_of text differs from decoded drive_state value.")
    timestamp = drive.get("timestamp")
    if isinstance(timestamp, bool) or not isinstance(timestamp, (float, int)):
        timestamp = None
    version = response.get("api_version")
    if isinstance(version, bool) or not isinstance(version, int):
        version = None
    return {"raw_gps_as_of": text, "decoded_gps_as_of": gps,
            "drive_state_timestamp": timestamp, "api_version": version,
            "coordinates_valid": True}


def fleet_get(url: str, access: str) -> tuple[int, bytes, dict]:
    if not url.startswith(REGIONS["NA"] + "/api/1/vehicles/"):
        raise SetupError("Non-allowlisted Fleet URL.")
    headers = {"Authorization": "Bearer " + access, "Accept": "application/json",
               "Accept-Encoding": "identity"}
    opener = urllib.request.build_opener(NoRedirect(), urllib.request.ProxyHandler({}),
                                         urllib.request.HTTPSHandler(context=ssl.create_default_context()))
    started = time.monotonic()
    try:
        with opener.open(urllib.request.Request(url, headers=headers), timeout=10) as response:
            body = response.read(MAX_BODY + 1)
            if len(body) > MAX_BODY or time.monotonic() - started > 20:
                raise SetupError("Fleet response exceeded size or time limit.")
            return response.status, body, {"date": response.headers.get("Date", ""),
                                           "x_txid": response.headers.get("x-txid", "")}
    except urllib.error.HTTPError as exc:
        raise SetupError(f"Fleet HTTP {exc.code}; body suppressed; no retry.") from None
    except (urllib.error.URLError, OSError, TimeoutError):
        raise SetupError("Fleet transport/TLS failure; no retry.") from None


def main() -> None:
    parser = argparse.ArgumentParser(description="Independent one-shot Fleet GPS diagnostic")
    parser.add_argument("--directory", type=Path, required=True,
                        help="Existing private onboarding directory containing setup.json")
    args = parser.parse_args()
    meta = json.loads((args.directory / "setup.json").read_text(encoding="utf-8"))
    if meta.get("region") != "NA":
        raise SetupError("This bounded diagnostic currently supports the North America region only.")
    client_id = meta["client_id"]
    redirect = validate_redirect(meta["redirect"])
    vin = input("Exact VIN to check (kept local): ").strip().upper()
    if not valid_vin(vin):
        raise SetupError("Invalid VIN.")
    print("This makes at most one status and one location request; no wake or commands.")
    print("The status/location requests may incur Fleet charges. No token is saved or refreshed.")
    if input("Type CHECK to begin fresh consent: ") != "CHECK":
        raise SetupError("Canceled before Tesla requests.")
    secret = getpass.getpass("Tesla client secret (hidden, used only for code exchange): ")
    state = secrets.token_urlsafe(32)
    authorization = AUTHORIZE_URL + "?" + urllib.parse.urlencode({
        "response_type": "code", "client_id": client_id, "redirect_uri": redirect,
        "scope": READ_SCOPES, "state": state, "nonce": secrets.token_urlsafe(32),
        "require_requested_scopes": "true", "prompt_missing_scopes": "true"})
    print("Open this read-only consent URL in your browser:\n" + authorization)
    callback = getpass.getpass("Paste the full HTTPS callback URL (hidden): ")
    try:
        code = callback_code(callback, redirect, state)
    except ValueError:
        raise SetupError("Malformed callback URL; details suppressed.") from None
    callback = ""
    token_response = request(TOKEN_URL, form={"grant_type": "authorization_code",
        "client_id": client_id, "client_secret": secret, "code": code,
        "audience": REGIONS["NA"], "redirect_uri": redirect})
    secret = code = ""
    access = token_response.get("access_token")
    if not isinstance(access, str) or not BEARER.fullmatch(access):
        raise SetupError("Authorization code exchange returned no access token.")
    if token_response.get("token_type") not in (None, "Bearer"):
        raise SetupError("Authorization code exchange returned a non-Bearer token.")
    scope = token_response.get("scope")
    if scope is not None and (not isinstance(scope, str) or
            not {"vehicle_device_data", "vehicle_location"}.issubset(set(scope.split()))):
        raise SetupError("Required read-only scopes were not granted.")
    token_response.clear()  # Discard any refresh token; the ESP32 remains its own refresh owner.
    base = REGIONS["NA"] + "/api/1/vehicles/" + vin
    status, raw, _ = fleet_get(base, access)
    if status != 200:
        raise SetupError("Vehicle status request failed.")
    vehicle = decode_json(raw)
    if not isinstance(vehicle, dict) or not isinstance(vehicle.get("response"), dict) or \
            vehicle["response"].get("vin") != vin:
        raise SetupError("Vehicle status VIN mismatch.")
    state_value = vehicle["response"].get("state")
    if state_value != "online":
        print("Vehicle is not ONLINE; no location request was made.")
        return
    began = dt.datetime.now(dt.timezone.utc)
    status, raw, headers = fleet_get(base + "/vehicle_data?endpoints=location_data", access)
    ended = dt.datetime.now(dt.timezone.utc)
    access = ""
    if status != 200:
        raise SetupError("Location request failed.")
    result = inspect_location(raw, vin)
    raw = b""  # Do not persist VIN, coordinates, or a full vehicle response.
    result.update({"source": "independent_laptop_fleet_rest", "endpoint": "location_data",
                   "http_status": status, "http_date": headers["date"],
                   "x_txid": headers["x_txid"],
                   "request_started_utc": began.isoformat(),
                   "response_completed_utc": ended.isoformat()})
    output = args.directory / "private" / ("gps-independent-" + ended.strftime("%Y%m%dT%H%M%SZ") + ".json")
    write_new(output, (json.dumps(result, indent=2) + "\n").encode())
    print("Independent raw-number check saved privately to:", output)
    print("gps_as_of raw text:", result["raw_gps_as_of"])
    print("No full response, VIN, coordinates, or tokens were saved.")


if __name__ == "__main__":
    try:
        main()
    except (SetupError, ValueError, KeyError, FileNotFoundError) as exc:
        print("Diagnostic stopped:", exc)
        raise SystemExit(1) from None

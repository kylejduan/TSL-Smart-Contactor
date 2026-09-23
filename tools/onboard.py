#!/usr/bin/env python3
"""Run only on the owner's laptop. No real secrets belong in command arguments."""
from __future__ import annotations
import argparse
import getpass
import json
import math
from pathlib import Path
import re
import sys
import urllib.parse
from tesla_setup import (SetupError, REGIONS, validate_redirect, register, consent, vehicles, select_vehicle)
from device_setup import prepare_files, write_new, usb_exchange

def hidden(prompt):
    if not sys.stdin.isatty():
        raise SetupError("Use an interactive terminal for hidden credential prompts.")
    return getpass.getpass(prompt)

def coordinate(prompt, limit):
    try:
        value = float(input(prompt).strip())
    except ValueError:
        raise SetupError("Invalid coordinate.") from None
    if not math.isfinite(value) or abs(value) > limit:
        raise SetupError("Coordinate out of range.")
    return value

def prepare(root):
    domain = input("Tesla application domain (no scheme/path): ").strip().lower()
    if not re.fullmatch(r"[a-z0-9](?:[a-z0-9.-]{0,251}[a-z0-9])?", domain) or "." not in domain:
        raise SetupError("Enter a public DNS domain you control.")
    redirect = validate_redirect(input("Exact registered HTTPS redirect URI: ").strip())
    host = input("Device LAN hostname or reserved IP (certificate SAN): ").strip().lower()
    if not re.fullmatch(r"[A-Za-z0-9.-]+", host):
        raise SetupError("Use a DNS hostname or IPv4 address; no scheme, port or path.")
    client_id = input("Tesla application client ID: ").strip()
    region = input("Region [NA/EU, default NA]: ").strip().upper() or "NA"
    if region not in REGIONS or not 1 <= len(client_id) <= 128:
        raise SetupError("Invalid client ID or region.")
    prepare_files(root, domain, host)
    write_new(root / "setup.json", json.dumps({"domain": domain, "redirect": redirect,
        "host": host, "client_id": client_id, "region": region}, indent=2).encode())
    print("Created private keys and per-device TLS certificate. Publish ONLY provisioning/public/ contents.")
    print("Place callback.html at your exact registered redirect path. Install local-ca.pem in your browser/OS trust store.")
    print("Reserve the chosen LAN IP/name. Keep public-key hosting available; never upload the private directory.")

def run(root, port):
    meta = json.loads((root / "setup.json").read_text())
    print("This performs Tesla registration/consent and read-only vehicle selection, then USB provisioning.")
    print("Disconnect contactor/mains wiring. It will replace configuration and leave output DISABLED, unarmed and dry-run.")
    if input("Type PROVISION to continue: ") != "PROVISION":
        raise SetupError("Canceled.")
    hello = usb_exchange(port, {"op": "hello"})
    if hello.get("protocol") != 1 or hello.get("board") != "ESP32-S3-Relay-1CH":
        raise SetupError("USB firmware protocol/board mismatch.")
    secret = hidden("Tesla application client secret (used here only): ")
    if input("Register this domain in the selected Tesla region now? [yes/no]: ") == "yes":
        register(meta["client_id"], secret, meta["domain"], meta["region"])
        print("Partner registration request succeeded.")
    tokens = consent(meta["client_id"], secret, meta["redirect"], meta["region"])
    secret = ""
    rows = vehicles(tokens["access_token"], meta["region"])
    print("VINs returned by Tesla:")
    for row in rows:
        print(row["vin"])
    vin = select_vehicle(rows, input("Type the exact VIN to authorize: ").strip())["vin"]
    lat = coordinate("Home latitude (decimal degrees): ", 90)
    lon = coordinate("Home longitude (decimal degrees): ", 180)
    ssid = hidden("Wi-Fi SSID: ")
    wifi = hidden("Wi-Fi password (WPA2/WPA3 personal): ")
    admin = hidden("New local administrator password (16-128 characters): ")
    if admin != hidden("Repeat administrator password: ") or not 16 <= len(admin.encode()) <= 128:
        raise SetupError("Administrator password mismatch or invalid length.")
    if not 1 <= len(ssid.encode()) <= 32 or not 8 <= len(wifi.encode()) <= 63:
        raise SetupError("Wi-Fi SSID/password length invalid.")
    payload = {"op": "provision", "confirmation": "REPLACE_CONFIG_USB_ONLY_MAINS_DISCONNECTED", "profile": {
        "config": {"vin": vin, "home_lat": lat, "home_lon": lon, "region": meta["region"]},
        "wifi_ssid": ssid, "wifi_password": wifi, "admin_password": admin,
        "client_id": meta["client_id"], "refresh_token": tokens["refresh_token"],
        "certificate": (root / "device-cert.pem").read_text(),
        "private_key": (root / "private" / "device-key.pem").read_text(),
        "origin": "https://" + meta["host"],
    }}
    result = usb_exchange(port, payload)
    if result.get("committed") is not True:
        raise SetupError("Device did not confirm durable provisioning; output remains inhibited. Recover via USB.")
    tokens.clear();payload.clear();admin = wifi = ""
    print("Token handed off. The ESP32 is now the sole refresh owner; this helper never refreshes the chain.")
    print("Device is rebooting DISABLED, uncommissioned and dry-run. Continue with docs/bench_checklist.md.")
    print("Python cannot guarantee RAM erasure; close this process and keep the laptop protected.")

def usb(args):
    request = {"op": args.command}
    if args.command not in {"hello", "status", "wifi_scan"}:
        request["password"] = hidden("Local administrator password: ")
    confirmations = {
        "arm": "USB_BENCH_POLARITY_AND_STARTUP_VERIFIED",
        "enable_output": "ALLOW_PHYSICAL_RELAY_AFTER_BENCH_CHECK",
    }
    if args.command in confirmations:
        phrase = confirmations[args.command]
        print("Read docs/bench_checklist.md. Contactor/mains wiring MUST be disconnected for bench work.")
        if input("Type " + phrase + ": ") != phrase:
            raise SetupError("Canceled.")
        request["confirmation"] = phrase
    if args.command == "timed_on":
        if not 1 <= args.seconds <= 28800:
            raise SetupError("Timed ON must be 1-28800 seconds; AUTO must already be selected.")
        if input("This bypasses Tesla presence. Type TIMED_ON to authorize: ") != "TIMED_ON":
            raise SetupError("Canceled.")
        request["seconds"] = args.seconds
    result = usb_exchange(args.port, request)
    if args.command == "wifi_scan" and result.get("ok"):
        for network in result["networks"]:
            network["ssid"] = bytes.fromhex(network["ssid_hex"]).decode("utf-8", errors="backslashreplace")
    print(json.dumps(result, indent=2))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=Path("provisioning"))
    commands = parser.add_subparsers(dest="operation", required=True)
    commands.add_parser("prepare", help="Generate new offline keys/certificates and setup metadata")
    runtime = commands.add_parser("run", help="Explicit Tesla registration/consent and USB provisioning")
    runtime.add_argument("--port", required=True)
    command = commands.add_parser("usb", help="Explicit USB management, no flashing")
    command.add_argument("--port", required=True)
    command.add_argument("command", choices=["hello", "status", "wifi_scan", "off", "auto", "timed_on", "arm", "enable_output", "reboot"])
    command.add_argument("--seconds", type=int, default=3600)
    args = parser.parse_args()
    try:
        if args.operation == "prepare": prepare(args.directory)
        elif args.operation == "run": run(args.directory, args.port)
        else: usb(args)
    except Exception as exc:
        # Only our vetted messages may include detail; third-party exception strings
        # can contain URLs, credential payloads, certificate data or local paths.
        print(str(exc) if isinstance(exc, SetupError) else "Setup failed; check local files/inputs. Sensitive details suppressed.", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Canceled; inspect USB status before repeating an uncertain handoff.", file=sys.stderr)
        return 130
    return 0

if __name__ == "__main__":
    raise SystemExit(main())

"""Bounded local HTTPS recorder; no Fleet calls or output/configuration actions."""
from __future__ import annotations

import argparse
import datetime as dt
import getpass
import hashlib
import http.client
import http.cookiejar
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import signal
import ssl
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

from device_setup import private_dir
from tesla_setup import NoRedirect, SetupError, decode_json

MAX_REPLY = 16384
MAX_LOG = 32 * 1024 * 1024
REASONS = set("uncommissioned user_disabled critical_local_fault no_auto_authorization "
              "auto_home_lease auto_home_retained auto_state_commit_pending timed_override_bypasses_presence minimum_off_dwell "
              "dry_run_output_inhibited utc_not_ready unknown".split())
ERRORS = set("none transport timeout response_too_large malformed_data gps_source_time_unusable "
             "vehicle_report_time_unusable authentication missing_permission rate_limit billing "
             "vehicle_unavailable server redirect_rejected local_request_cap storage_fault "
             "reauthorization_required utc_not_ready wifi_unavailable unknown".split())
ENUMS = {"mode": {"AUTO", "DISABLED", "TIMED_ON"}, "gpio_command": {"ON commanded", "OFF commanded"},
         "reason": REASONS, "vehicle": {"online", "asleep", "offline", "unknown"},
         "position_basis": {"gps_source", "vehicle_report"}, "outage_policy": {"expire", "hold_last"}, "error": ERRORS,
         "fleet_endpoint": {"none", "status", "location", "refresh", ""}}
BOOLEANS = set("desired_on ready commissioned dry_run fault auto_home auto_retained auto_restored auto_state_pending wifi_connected utc_ready "
               "reauthorization_needed poll_busy".split())
INTEGERS = set("generation uptime_s lease_s override_s location_age_s next_poll_s last_success_uptime_s "
               "rssi_dbm control_max_gap_ms internal_heap_free fleet_http_status "
               "fleet_received_utc_s location_monthly_cap".split())
NUMBERS = {"distance_m", "reported_distance_m", "estimated_location_usd"}
COUNTS = {"attempts_this_boot", "reserved_today", "reserved_month"}


class ObserverError(Exception):
    """Only fixed, non-secret labels are exposed to logs."""
    def __init__(self, code: str, fatal: bool = False):
        self.code, self.fatal = code, fatal
        super().__init__(code)


def origin_url(value: str) -> str:
    try:
        p = urllib.parse.urlsplit(value)
        ip = ipaddress.ip_address(p.hostname or "")
        networks = ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")
        local = ip.version == 4 and any(ip in ipaddress.ip_network(n) for n in networks)
        valid = (p.scheme == "https" and local and p.port in (None, 443)
                 and not p.username and not p.password and p.path in ("", "/")
                 and not p.query and not p.fragment and not any(c.isspace() for c in value))
    except ValueError:
        valid = False
    if not valid:
        raise ObserverError("expected_private_ipv4_https_origin", True)
    return "https://" + p.hostname


def password_from_file(path: Path) -> str:
    try:
        if path.stat().st_size > 4096:
            raise ObserverError("invalid_password_file", True)
        entries = [line.partition("=")[2].strip() for line in path.read_text(encoding="utf-8").splitlines()
                   if line.partition("=")[0].strip() == "LOCAL" and "=" in line]
    except (OSError, UnicodeError):
        raise ObserverError("password_file_unreadable", True) from None
    if len(entries) != 1 or not 16 <= len(entries[0].encode("utf-8")) <= 128:
        raise ObserverError("expected_one_LOCAL_password", True)
    return entries[0]


def safe_status(document: dict) -> dict:
    if not isinstance(document, dict) or not all(k in document for k in ("mode", "uptime_s", "gpio_command")):
        raise ObserverError("invalid_status")
    result = {}
    for key, allowed in ENUMS.items():
        if key in document:
            value = document[key]
            if not isinstance(value, str) or value not in allowed:
                raise ObserverError("invalid_status")
            result[key] = value
    for key in BOOLEANS | INTEGERS | NUMBERS | COUNTS:
        if key not in document:
            continue
        value = document[key]
        valid = (type(value) is bool if key in BOOLEANS else
                 type(value) is int and abs(value) < 2**63 if key in INTEGERS else
                 type(value) in (int, float) and math.isfinite(value) if key in NUMBERS else
                 isinstance(value, list) and len(value) == 3 and
                 all(type(v) is int and 0 <= v <= 2**32 - 1 for v in value))
        if not valid:
            raise ObserverError("invalid_status")
        result[key] = value
    for key in ("gps_source_text", "report_timestamp_text"):
        value = document.get(key)
        if isinstance(value, str) and (not value or re.fullmatch(r"-?[0-9.eE+\-]{1,63}", value)):
            result[key] = value
    for key in ("fault_source", "fleet_detail", "firmware_version", "sdk_version"):
        value = document.get(key)
        if isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9_.\-]{1,64}", value):
            result[key] = value
    settings = document.get("settings", {})
    if isinstance(settings, dict):
        result["policy"] = {k: v for k, v in settings.items()
                            if k in {"max_age_s", "lease_s", "poll_s", "enable_m", "disable_m", "sleep_s"}
                            and type(v) is int and 0 <= v <= 86400}
    # VIN, coordinates, origin, credentials, cookies and tokens are never retained.
    return result


def safe_events(document: dict) -> list:
    events = document.get("events") if isinstance(document, dict) else None
    if not isinstance(events, list) or len(events) > 16:
        raise ObserverError("invalid_events")
    result = []
    for event in events:
        if not isinstance(event, dict) or not (type(event.get("uptime_s")) is int
                and 0 <= event["uptime_s"] < 2**63 and type(event.get("commanded_on")) is bool
                and isinstance(event.get("reason"), str) and event["reason"] in REASONS):
            raise ObserverError("invalid_events")
        result.append({k: event[k] for k in ("uptime_s", "reason", "commanded_on")})
    return result


class LocalClient:
    def __init__(self, origin: str, ca: Path, password: str):
        self.origin = origin_url(origin)
        self.password = password
        self.csrf = ""
        self.authenticated = False
        self.opener = urllib.request.build_opener(NoRedirect(), urllib.request.ProxyHandler({}),
            urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()),
            urllib.request.HTTPSHandler(context=ssl.create_default_context(cafile=str(ca))))

    def request(self, path: str, payload=None) -> dict:
        permitted = (payload is None and path in ("/api/login-info", "/api/status", "/api/events")
                     or path == "/api/login" and isinstance(payload, dict) and set(payload) == {"material"}
                     or path == "/api/action" and payload == {"action": "logout"})
        if not permitted:
            raise ObserverError("observer_action_rejected", True)
        data = None if payload is None else json.dumps(payload).encode()
        request = urllib.request.Request(self.origin + path, data=data, headers={
            "Origin": self.origin, "Content-Type": "application/json", "X-CSRF-Token": self.csrf,
            "Accept-Encoding": "identity"})
        started = time.monotonic()
        try:
            with self.opener.open(request, timeout=10) as response:
                if response.headers.get("Content-Encoding", "identity") != "identity":
                    raise ObserverError("unsupported_encoding")
                raw = bytearray()
                while True:
                    if time.monotonic() - started > 30:
                        raise ObserverError("local_timeout")
                    chunk = response.read1(min(4096, MAX_REPLY + 1 - len(raw)))
                    if not chunk:
                        break
                    raw.extend(chunk)
                    if len(raw) > MAX_REPLY:
                        raise ObserverError("local_response_too_large")
                document = decode_json(bytes(raw))
                csrf = response.headers.get("X-CSRF-Token", "")
                if re.fullmatch(r"[0-9a-f]{64}", csrf):
                    self.csrf = csrf
                if not isinstance(document, dict):
                    raise ObserverError("invalid_reply")
                return document
        except urllib.error.HTTPError as exc:
            status = exc.code
            exc.close()
            raise ObserverError("local_http_" + str(status)) from None
        except (urllib.error.URLError, OSError, TimeoutError, http.client.HTTPException):
            raise ObserverError("local_transport_or_tls") from None
        except (SetupError, ValueError, RecursionError):
            raise ObserverError("invalid_reply_or_redirect") from None

    def login(self):
        info = self.request("/api/login-info")
        if (info.get("version") != 2 or info.get("iterations") != 100000 or
                not isinstance(info.get("salt"), str) or not re.fullmatch(r"[0-9a-f]{32}", info["salt"])):
            raise ObserverError("unsupported_authentication_profile", True)
        material = hashlib.pbkdf2_hmac("sha256", self.password.encode(), bytes.fromhex(info["salt"]),
                                     100000, dklen=32).hex()
        try:
            result = self.request("/api/login", {"material": material})
        except ObserverError as exc:
            if exc.code in {"local_http_400", "local_http_401", "local_http_403"}:
                raise ObserverError("local_login_rejected", True) from None
            raise
        finally:
            material = ""
        if result.get("ok") is not True:
            raise ObserverError("local_login_rejected", True)
        self.authenticated = True

    def snapshot(self) -> dict:
        if not self.authenticated:
            self.login()
        try:
            document = self.request("/api/status")
        except ObserverError as exc:
            if exc.code != "local_http_401":
                raise
            self.authenticated = False
            self.login()  # One re-login, then one status retry; no authentication storm.
            document = self.request("/api/status")
        result = {"status": safe_status(document)}
        try:
            result["events"] = safe_events(self.request("/api/events"))
        except ObserverError as exc:
            result["events_error"] = exc.code
        return result

    def close(self) -> str:
        try:
            if self.authenticated and self.csrf:
                self.request("/api/action", {"action": "logout"})
                return "logged_out"
            return "no_session"
        except ObserverError:
            return "logout_not_acknowledged"
        finally:
            self.authenticated = False
            self.password = ""
            self.csrf = ""


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def record(client, output: Path, duration_s: float, interval_s: int, stop: threading.Event) -> int:
    private_dir(output.parent)
    flags = os.O_CREAT | os.O_EXCL | os.O_WRONLY | getattr(os, "O_NOFOLLOW", 0)
    deadline = time.monotonic() + duration_s
    previous = None
    failures = 0
    exit_code = 0
    with os.fdopen(os.open(output, flags, 0o600), "w", encoding="utf-8") as file:
        def append(entry):
            row = json.dumps(entry, separators=(",", ":"), allow_nan=False) + "\n"
            if file.tell() + len(row.encode()) > MAX_LOG:
                raise ObserverError("local_log_size_limit", True)
            file.write(row)
            file.flush()
            os.fsync(file.fileno())
        append({"kind": "start", "utc": utc_now(), "duration_s": duration_s, "interval_s": interval_s})
        try:
            while not stop.is_set() and time.monotonic() < deadline:
                started = time.monotonic()
                entry = {"kind": "sample", "utc": utc_now()}
                delay = interval_s
                try:
                    entry.update(client.snapshot())
                    status = entry["status"]
                    if previous is not None:
                        entry["uptime_decreased"] = status["uptime_s"] < previous["uptime_s"]
                        entry["generation_changed"] = status.get("generation") != previous.get("generation")
                    previous = status
                    failures = 0
                except ObserverError as exc:
                    entry["observer_error"] = exc.code
                    failures += 1
                    delay = max(interval_s, min(300, 30 * 2**min(failures, 4)))
                    if exc.code == "local_http_429":
                        delay = max(delay, 300)
                    if exc.fatal:
                        exit_code = 1
                entry["finished_utc"] = utc_now()
                append(entry)
                print(json.dumps({k: entry[k] for k in ("utc", "observer_error") if k in entry}
                      | {"sample_saved": True}), flush=True)
                if exit_code:
                    break
                stop.wait(max(0, min(deadline - time.monotonic(), started + delay - time.monotonic())))
        except ObserverError as exc:
            print("Recorder stopped: " + exc.code, flush=True)
            exit_code = 1
        finally:
            cleanup = client.close()
            if file.tell() < MAX_LOG - 512:
                append({"kind": "end", "utc": utc_now(), "cleanup": cleanup,
                        "interrupted": stop.is_set(), "exit_code": exit_code})
    return exit_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--origin", required=True, help="Device HTTPS origin using its reserved private IPv4 address")
    parser.add_argument("--ca", type=Path, required=True, help="Per-device local CA certificate")
    parser.add_argument("--password-file", type=Path, help="Protected local file containing one LOCAL= entry; otherwise prompt")
    parser.add_argument("--output-dir", type=Path, default=Path(".tools/observations"))
    parser.add_argument("--duration-hours", type=float, default=16)
    parser.add_argument("--interval-s", type=int, default=60)
    args = parser.parse_args()
    if not math.isfinite(args.duration_hours) or not 0 < args.duration_hours <= 48 or not 30 <= args.interval_s <= 600:
        parser.error("Duration must be >0 and <=48 hours; interval must be 30..600 seconds.")
    stop = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda _sig, _frame: stop.set())
    client = None
    try:
        if os.name == "posix":
            import resource
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        password = password_from_file(args.password_file) if args.password_file else getpass.getpass("Local administrator password: ")
        if not 16 <= len(password.encode()) <= 128:
            raise ObserverError("invalid_local_password_length", True)
        client = LocalClient(args.origin, args.ca, password)
        password = ""
        stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
        output = args.output_dir / ("controller-" + stamp + ".jsonl")
        print("Local recorder only; no Tesla requests or relay/configuration actions. Keep this computer awake.", flush=True)
        print("One admin session: recorder login/renewal can sign out the browser dashboard.", flush=True)
        print("Observation file: " + str(output), flush=True)
        return record(client, output, args.duration_hours * 3600, args.interval_s, stop)
    except ObserverError as exc:
        print("Recorder failed: " + exc.code)
    except (OSError, ValueError):
        print("Recorder failed: local_setup_or_storage; details suppressed.")
    finally:
        if client is not None:
            client.close()
    return 1


if __name__ == "__main__":
    raise SystemExit(main())

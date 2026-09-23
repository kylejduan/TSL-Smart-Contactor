"""Official Tesla onboarding only; never stores account passwords or refreshes tokens.
All network calls are invoked explicitly by the CLI, never at import/test time.
"""
from __future__ import annotations
import getpass
import json
import secrets
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

REGIONS = {
    "NA": "https://fleet-api.prd.na.vn.cloud.tesla.com",
    "EU": "https://fleet-api.prd.eu.vn.cloud.tesla.com",
}
TOKEN_URL = "https://fleet-auth.prd.vn.cloud.tesla.com/oauth2/v3/token"
AUTHORIZE_URL = "https://auth.tesla.com/oauth2/v3/authorize"
SCOPES = "openid offline_access vehicle_device_data vehicle_location"
MAX_BODY = 262144

class SetupError(Exception):
    """A deliberately redacted, user-safe diagnostic."""

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise SetupError("Redirect rejected. Check the documented endpoint/region.")

def validate_redirect(uri: str) -> str:
    p = urllib.parse.urlsplit(uri)
    if (p.scheme != "https" or not p.hostname or p.username or p.password or
            p.query or p.fragment or p.hostname in {"localhost", "127.0.0.1", "::1"} or
            any(c.isspace() for c in uri) or "\\" in uri):
        raise SetupError("Use an exact registered static HTTPS redirect, without query/fragment.")
    return uri

def callback_code(callback: str, redirect: str, state: str) -> str:
    expected = urllib.parse.urlsplit(validate_redirect(redirect))
    supplied = urllib.parse.urlsplit(callback)
    if ((supplied.scheme, supplied.netloc, supplied.path) !=
            (expected.scheme, expected.netloc, expected.path) or supplied.fragment):
        raise SetupError("Callback location did not match the registered redirect exactly.")
    pairs = urllib.parse.parse_qs(supplied.query, keep_blank_values=True, strict_parsing=True)
    if any(len(v) != 1 for v in pairs.values()):
        raise SetupError("Duplicate callback parameters rejected.")
    if not secrets.compare_digest(pairs.get("state", [""])[0], state):
        raise SetupError("OAuth state mismatch; begin a new consent flow.")
    if "error" in pairs or not pairs.get("code", [""])[0]:
        raise SetupError("Tesla consent failed or returned no authorization code.")
    return pairs["code"][0]

def authorization_url(client_id: str, redirect: str, state: str) -> str:
    return AUTHORIZE_URL + "?" + urllib.parse.urlencode({
        "response_type": "code", "client_id": client_id,
        "redirect_uri": validate_redirect(redirect), "scope": SCOPES,
        "state": state, "nonce": secrets.token_urlsafe(32),
        "require_requested_scopes": "true", "prompt_missing_scopes": "true",
    })

def decode_json(raw: bytes) -> Any:
    def pairs(items):
        result = {}
        for k, v in items:
            if k in result:
                raise SetupError("Duplicate API JSON member rejected.")
            result[k] = v
        return result
    def invalid(_):
        raise SetupError("Non-finite JSON number rejected.")
    if len(raw) > MAX_BODY:
        raise SetupError("API response exceeds the setup helper limit.")
    try:
        value = json.loads(raw, object_pairs_hook=pairs, parse_constant=invalid)
        def walk(v, depth=0):
            if depth > 12:
                raise SetupError("API JSON nesting limit exceeded.")
            if isinstance(v, dict):
                for x in v.values(): walk(x, depth + 1)
            elif isinstance(v, list):
                for x in v: walk(x, depth + 1)
        walk(value)
        return value
    except (ValueError, RecursionError, UnicodeError) as exc:
        raise SetupError("Malformed API response; details suppressed.") from None

def request(url: str, *, token: str | None = None, form=None, body=None):
    allowed = set(REGIONS.values()) | {"https://fleet-auth.prd.vn.cloud.tesla.com"}
    p = urllib.parse.urlsplit(url)
    if f"{p.scheme}://{p.netloc}" not in allowed or p.username or p.fragment:
        raise SetupError("Non-allowlisted Tesla URL rejected.")
    headers = {"Accept": "application/json"}
    data = None
    if token:
        headers["Authorization"] = "Bearer " + token
    if form is not None:
        data = urllib.parse.urlencode(form).encode()
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    opener = urllib.request.build_opener(NoRedirect(), urllib.request.HTTPSHandler(context=ssl.create_default_context()))
    try:
        with opener.open(urllib.request.Request(url, data=data, headers=headers), timeout=10) as response:
            started = time.monotonic()
            chunks = bytearray()
            while True:
                if time.monotonic() - started > 20:
                    raise SetupError("Setup request timed out; no automatic retry was made.")
                chunk = response.read(min(4096, MAX_BODY + 1 - len(chunks)))
                if not chunk:
                    break
                chunks.extend(chunk)
                if len(chunks) > MAX_BODY:
                    raise SetupError("Setup response too large.")
            value = decode_json(bytes(chunks))
            if not isinstance(value, dict):
                raise SetupError("API response is not an object.")
            return value
    except urllib.error.HTTPError as exc:
        raise SetupError(f"Tesla HTTP {exc.code}; response body suppressed. No automatic retry.") from None
    except (urllib.error.URLError, OSError, TimeoutError):
        raise SetupError("Tesla transport/TLS failure; details suppressed. No automatic retry.") from None

def register(client_id: str, client_secret: str, domain: str, region: str):
    token = request(TOKEN_URL, form={"grant_type": "client_credentials", "client_id": client_id,
        "client_secret": client_secret, "audience": REGIONS[region],
        "scope": "openid vehicle_device_data vehicle_location"})
    if not isinstance(token.get("access_token"), str):
        raise SetupError("Partner token response missing an access token.")
    request(REGIONS[region] + "/api/1/partner_accounts", token=token["access_token"], body={"domain": domain})

def consent(client_id: str, client_secret: str, redirect: str, region: str):
    state = secrets.token_urlsafe(32)
    print("Open this consent URL in your browser:\n" + authorization_url(client_id, redirect, state))
    callback = getpass.getpass("Paste the full HTTPS callback URL (hidden): ")
    try:
        code = callback_code(callback, redirect, state)
    except ValueError:
        raise SetupError("Malformed callback URL.") from None
    result = request(TOKEN_URL, form={"grant_type": "authorization_code", "client_id": client_id,
        "client_secret": client_secret, "code": code, "audience": REGIONS[region], "redirect_uri": redirect})
    if not all(isinstance(result.get(k), str) and result[k] for k in ("access_token", "refresh_token")):
        raise SetupError("Code exchange response lacks tokens; begin a new consent flow.")
    if "scope" in result and not {"vehicle_device_data", "vehicle_location"}.issubset(set(result["scope"].split())):
        raise SetupError("Read-only vehicle/location scopes were not granted.")
    return result

def vehicles(access: str, region: str):
    result = []
    for page in range(1, 101):
        data = request(REGIONS[region] + f"/api/1/vehicles?page={page}&per_page=100", token=access)
        rows = data.get("response")
        if not isinstance(rows, list):
            raise SetupError("Unexpected vehicle-list schema.")
        for row in rows:
            if not isinstance(row, dict) or not valid_vin(row.get("vin")):
                raise SetupError("Vehicle list contains an invalid VIN.")
            result.append(row)
        pagination = data.get("pagination", {})
        if not pagination.get("next"):
            return result
    raise SetupError("Vehicle pagination limit exceeded; select a smaller account scope.")

def valid_vin(vin):
    return isinstance(vin, str) and len(vin) == 17 and all(c in "0123456789ABCDEFGHJKLMNPRSTUVWXYZ" for c in vin)

def select_vehicle(rows, vin):
    matches = [r for r in rows if r["vin"] == vin]
    if len(matches) != 1:
        raise SetupError("Enter exactly one VIN returned by Tesla; no vehicle is selected by default.")
    return matches[0]

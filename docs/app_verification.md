# Local application verification

The dashboard distinguishes AUTO authorization, desired output and ON/OFF
commands; it does not confirm outlet voltage or charging. Commissioning, dry-run,
faults, selected timestamp basis and reauthorization are visible.

Synthetic browser tests cover login errors, commissioning gates, confirmation,
OFF during pending work, draft preservation and validation, hostile text,
redaction, session expiry, sign-out, unavailable history, absence of background
paid requests, external resources and browser credential storage. Run the
commands in [local app](local_app.md#reproducible-browser-verification).

Password profile 2 uses browser PBKDF2-SHA256 with 100,000 rounds, followed by a
domain-separated verifier and constant-time comparison on the ESP32. The derived
material is a reusable password equivalent and must travel only over trusted
per-device HTTPS. A legacy profile migrates only after successful authentication
and checked durable storage. USB verification still performs the slower derivation.
Sessions use a random Secure/HttpOnly/SameSite cookie, exact Origin and independent
CSRF token, login throttling and a monotonic expiry. No browser storage is used.

The work factor is below the cited
[OWASP PBKDF2 recommendation](https://cheatsheetseries.owasp.org/cheatsheets/Password_Storage_Cheat_Sheet.html).
Use a unique high-entropy 16–128-byte administrator password. Plain NVS and
unencrypted flash do not prevent physical credential extraction. Login latency
is browser/device/network dependent; software does not promise a universal bound.

USB Wi-Fi recovery preserves the token and TLS keys, requires an authenticated
local action while uncommissioned, DISABLED and dry-run, and checks NVS commits.
Live measurements belong in private installation records. Offline UI checks do
not replace relay, outbound TLS, Wi-Fi outage or physical provisioning acceptance.

# Local application completion checks

Recorded 2026-09-23 local / 2026-09-24 UTC. This records the dashboard revision,
not physical commissioning or resolution of the Tesla GPS timestamp anomaly.

## Delivered behavior

- Responsive overview distinguishes commanded GPIO, AUTO lease, reported position,
  and source-time acceptance. Commissioning, dry-run and local faults are explicit.
- AUTO, timed override and settings changes require confirmation. OFF is available
  in those dialogs and during other pending requests. Late browser responses cannot
  overwrite a newer OFF result.
- Settings have numeric bounds and cross-field validation. Empty coordinates cannot
  become zero. Refresh preserves unsaved edits; Discard and acknowledged Save reset
  the draft. Physical-output enablement remains USB-only.
- Authenticated decision history exposes the existing bounded RAM log, newest first.
  The ring retains 16 transitions without dependence on a wrapping total count.
- A redacted downloadable report excludes identity, coordinates and credentials.
  Fault recovery, reauthorization and USB commands are available on the page.
- Sign-out works during a local fault and expires the cookie. Check-now returns a
  clear rejection when DISABLED, disconnected, clock-not-ready, busy or in backoff.
- Separate embedded HTML/CSS/JS permit a same-origin CSP without inline scripts or
  styles. Session expiry clears private page fields. No background browser API
  polling, external page resources, or browser storage was introduced.

## Executed checks

| Check | Result |
| --- | --- |
| `cmake --build build-host && ctest --test-dir build-host --output-on-failure` | PASS: 58 native tests, ASan/UBSan enabled |
| `.venv/bin/python -m unittest discover -s tests -p 'test_*.py' -q` | PASS: 12 Python tests |
| `node --check main/app.js` | PASS |
| Pinned Playwright 1.58.2 browser suite | PASS: 9 scenarios, synthetic loopback responses only |
| agent-browser 0.38.1 desktop interaction and screenshots | PASS: sign-in, page structure and controls inspected; no browser errors |
| Desktop 1280 px / mobile 390 px | PASS: screenshots inspected, no horizontal document overflow |
| ESP-IDF v5.5.2 `idf.py build` | PASS: 981,472-byte application, 69% app-partition space remaining |
| GitHub Actions run `35966899232`, revision `f7a5113` | PASS: native, browser and esp32s3 jobs |
| `git diff --check` | PASS |

Application SHA-256:
`c0c89373f9d33b66a6256e2baaa4c847d0fbf82ae9d3cec8663eddf3694ce05c`

The browser suite covers login errors, commissioning gates, confirmation/cancel,
OFF from a confirmation dialog and while status is delayed, settings limits and
hidden invalid fields, draft preservation, redaction, hostile text rendering,
fault-state sign-out, unavailable event history, session expiry, no background
requests, no external resources and no localStorage/sessionStorage entries.
See [local app guide](local_app.md#reproducible-browser-verification) for commands.
Node v24.18.0 and the Ubuntu 24.04 Chromium build were used on this Ubuntu 26.04
host. Browser dependencies are test-only and pinned in the test lockfile.

## Installed-board verification, 2026-09-24 UTC

The controller was unavailable when the update was ready: Windows enumerated no
serial ports, and the previously configured HTTPS address did not answer. The owner
then reconnected USB and confirmed mains/contactor wiring remained disconnected.
Pre-update USB checks confirmed DISABLED, uncommissioned, dry-run, OFF commanded,
no fault, intact profile and a usable token journal.

The image above was written to the application partition at `0x30000`, using
esptool 4.12.0; flash hash verification passed. Bootloader, partition table and NVS
were preserved. An initial HTTPS attempt before Wi-Fi was ready timed out. At
45 seconds uptime USB confirmed Wi-Fi and UTC readiness, and the subsequent
Windows-native Python check passed using normal certificate/hostname verification:

- Served HTML, CSS and JavaScript matched the source files byte for byte.
- CSP required same-origin scripts without `unsafe-inline`.
- Unauthenticated status/event reads returned 401; authenticated login succeeded.
- Session lifetime and bounded event history were present; both recorded events
  showed OFF commanded.
- Check-now and timed override returned 409 while DISABLED. Invalid coordinates
  returned 400; attempting to disable dry-run through settings returned 409.
- Authenticated OFF succeeded; logout invalidated the session.
- Exact attempt counters remained `[0, 0, 0]`: no token, status or location calls.

Final USB at 87 seconds uptime confirmed DISABLED, uncommissioned, dry-run,
OFF commanded, intact profile, usable token journal, Wi-Fi/UTC ready and no fault
or allocation failure. Maximum observed control gap was **57 ms**, free internal
heap **79,060 bytes**, largest block **31,744 bytes**. This is an observed interval,
not a worst-case timing guarantee. Local verification processes exited normally.

Browser interaction/screenshots above used synthetic responses; installed-board
verification used the actual HTTPS endpoints and exact embedded assets. No live
Tesla request, relay ON command, arming or output enablement was performed for this
dashboard work. Earlier image results remain in [verification.md](verification.md).

## Remaining hardware and vehicle boundary

The known negative GPS source-time issue still prevents valid live AUTO acceptance.
Supervised polarity, continuity, boot-pulse, watchdog and brownout checks remain
separate; browser/native tests do not satisfy those measurements.

## Faster password login, 2026-09-24 UTC

The password remains the administrator credential. The v2 profile stores a
domain-separated SHA-256 verifier of the existing 100,000-round PBKDF2 output.
Web Crypto performs PBKDF2 in the browser; the ESP32 does the final digest and
constant-time comparison. The public login-info response contains only the record
version, salt and work factor. The derived material is a reusable password
equivalent in transit, protected by the same trusted local HTTPS as the former raw
password. Nothing is saved in browser storage. A v1 profile migrates on the first
successful login using a single durable NVS profile write; failed writes inhibit
output. USB password checks still take the slower path.

ESP-IDF v5.5.2 built a **990,784-byte** application, SHA-256
`a5bd71d2f8795566a791c25ea6abe3715f3794127858df9d01af0dd1b87be12b`.
The 58 native tests, 12 Python tests, JavaScript syntax check and 9 synthetic
browser scenarios passed. The browser fixture rejects a wrong derived password and
asserts that sign-in sends derived material rather than the raw password.

With the board DISABLED, uncommissioned, dry-run and OFF commanded, esptool 4.12.0
verified the app-only flash hash at `0x30000`, preserving NVS. A trusted Windows
HTTPS check observed v1, then a **9.013 s** first login and durable migration to
v2. The next login request completed in **0.665 s**. After an authenticated reboot,
USB reported an intact profile and usable token journal; v2 remained selected and
another login request completed in **0.752 s**. A wrong derived value returned 401,
and its immediate retry returned 429. Final authenticated state remained DISABLED,
uncommissioned, dry-run, OFF commanded, with no fault, maximum observed control
gap **51 ms**, and Fleet attempt counters `[0, 0, 0]`. These are measurements on
this PC and LAN, not a guaranteed maximum on every browser or network.

## Monthly Fleet data budget, 2026-09-24 UTC

Tesla published 500 Data requests/$1 and a $10 monthly account discount at review.
This revision limits the controller to 5,000 reserved live-data attempts per UTC
month and delays repeated check-now requests by 10 minutes. Automatic polling stays
at 600 seconds to permit renewal of the 900-second authorization lease. The budget
test covers month rollover, reboot, cap exhaustion before status or refresh, and
lease expiry without renewal.

`cmake --build build-host && ctest --test-dir build-host --output-on-failure`
passed 61 native tests; 12 Python tests, `node --check main/app.js`, and nine
synthetic browser scenarios passed. ESP-IDF v5.5.2 built a 991,264-byte image,
SHA-256 `dd51890e3b8acc4883da543c4b814eb4e4933a7aa7974eee053ef24002ab6b20`.
Esptool 4.12.0 verified an application-only flash at `0x30000`, leaving NVS intact.

USB confirmed the board booted DISABLED, uncommissioned, dry-run, OFF commanded,
without a fault; the profile and token journal remained intact. Once Wi-Fi and UTC
were ready, trusted local HTTPS login completed in 0.661 seconds and reported
`location_monthly_cap=5000`, `poll_s=600`, and Fleet attempt counters `[0, 0, 0]`.
No live Tesla request or relay energization was used for this budget check. Tesla's
account-level spending limit and other applications' usage were not changed or
verified here; the controller's local estimate remains pre-discount.

## Password boundary review, 2026-09-24 UTC

The fast-login profile stores a salted PBKDF2-SHA256-derived verifier, not the
password or the browser-sent derived value. The browser sends the derived value
only over per-device HTTPS; that value is reusable, so certificate validation is
essential. The 256-bit random session cookie is Secure, HttpOnly, SameSite=Strict
and expires after 15 minutes. State changes require the cookie, exact Origin and
an independent CSRF token. Failed login attempts are throttled; the browser saves
no password or derived material in persistent storage.

This review closed the legacy HTTPS raw-password path after v1-to-v2 migration.
The installed v2 controller rejected a synthetic raw-password request with 401
in 0.816 seconds, without starting its slow PBKDF2 check. A subsequent normal
derived-material login succeeded in 0.726 seconds over trusted HTTPS. Final status
remained DISABLED, uncommissioned, dry-run, OFF commanded, without a fault or
Fleet requests. The app-only flash hash was verified; NVS was preserved.

The 100,000-iteration work factor is below [OWASP's current 600,000-iteration
PBKDF2-HMAC-SHA256 recommendation](https://cheatsheetseries.owasp.org/cheatsheets/Password_Storage_Cheat_Sheet.html)
for password databases. A unique, high-entropy
16-128-byte administrator password and local-only access remain necessary. Plain
NVS and unencrypted flash do not protect the verifier, Tesla token, Wi-Fi secret
or TLS private key from someone with physical extraction access. A compromised
browser, its trusted CA, or the local computer can also expose login material.

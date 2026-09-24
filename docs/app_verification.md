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

## Hardware boundary

The controller was unavailable when the update was ready: Windows enumerated no
serial ports, and the previously configured HTTPS address did not answer. This
revision has **not yet been flashed or verified on the board**. Prior hardware
results in [verification.md](verification.md) apply to their recorded older images.
An application-only update and local HTTPS/USB verification are prepared; they
preserve NVS and the device-owned refresh-token chain. No live Tesla request, relay
ON command, arming or output enablement was performed for this dashboard work.

The known negative GPS source-time issue still prevents valid live AUTO acceptance.
Supervised polarity, continuity, boot-pulse, watchdog and brownout checks remain
separate; browser/native tests do not satisfy those measurements.

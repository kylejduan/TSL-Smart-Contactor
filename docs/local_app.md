# Local application

Open the controller's configured **HTTPS** LAN address. All page assets are served
by the ESP32; there are no CDNs, map providers, external fonts, telemetry scripts,
service workers or browser credential storage. The per-device certificate and
local CA are generated during USB preparation. Port 80 is not served.

## Reading the dashboard

- **ON commanded / OFF commanded** is the last recorded GPIO command, never proof
  of receptacle voltage or charging.
- **AUTO permission** is the independent source-anchored HOME lease. Manual override
  and dry-run do not create this permission.
- **Reported position** is the distance from returned coordinates to configured
  home, labeled freshness unverified. An invalid timestamp cannot authorize AUTO.
- **Location evidence** distinguishes an accepted fix from missing or invalid GPS
  source time. General report timestamps are diagnostic only.
- **Readiness** lists profile/clock/network, physical bench acknowledgement,
  physical-output enablement and AUTO evidence separately.

A status refresh reads only the controller's snapshot. No browser tab polls Tesla.
Countdowns are estimates from that snapshot and use browser monotonic elapsed time;
when a timer reaches zero, refresh to confirm the current commanded state. Keeping
an old page open does not keep an authorization alive. Sessions expire 15 minutes
after sign-in; page reload can restore a still-valid HttpOnly session.

## Controls

OFF is always available while signed in, including during another pending request
and inside confirmation dialogs. It inhibits output, cancels overrides and saves
DISABLED. A timeout means the browser did not confirm the outcome: inspect a fresh
snapshot or use authenticated USB OFF. Do not assume success from a spinner.

AUTO requires an explicit confirmation because it permits recurring Fleet requests
and, after commissioning, authorization. TIMED_ON also requires confirmation, uses
a one-hour default and an eight-hour maximum, and cannot start while DISABLED or
uncommissioned. It bypasses Tesla presence/connectivity but not local faults or
minimum OFF dwell. These gates are enforced by firmware, not just disabled buttons.

Check now uses the same scheduler as automatic polling. It rejects DISABLED,
missing Wi-Fi/UTC, active requests, and current backoff/rate limits. An accepted
request is queued, not a claim of a successful Tesla observation. Refresh local
status to see its result. No vehicle wake or command endpoint is called.

## Settings and recovery

Numeric fields have explicit limits matching the production configuration checks.
Empty coordinates are invalid and cannot become zero by form conversion. Disable
radius must exceed enable radius; sleep ceiling must be at least the ordinary
lease. Drafts survive status refresh; Discard edits reloads the last fetched
settings. Save asks for confirmation, clears old evidence/overrides, and retains
the persistent AUTO/DISABLED selection. Fresh evidence is required to resume AUTO.

The browser may enter dry-run. Arming, leaving dry-run, secret replacement and
network/certificate provisioning remain USB-only. The page includes the exact
helper commands and points to the physical bench checklist. Login verification
can take about 10 seconds; the browser allows 30 seconds and prevents duplicate
submissions. Sign-out remains available when a local fault requires recovery.

## Diagnostics and privacy

The 16 most recent control reason/command transitions appear newest first. Their
times are uptime, not wall-clock times; the log is RAM-only and lost on reboot.
It is not an audit of relay contacts or every network operation.

The downloadable JSON report contains a deliberately allowlisted snapshot and
control events. It omits VIN, home/GPS coordinates, reported distance, application
client ID, LAN address, passwords, tokens and cookies. It includes Tesla request
IDs and timestamp tokens to support debugging. Authenticated settings still show
VIN/home to the owner. Review an export before sharing; no export is uploaded by
the page. No credentials or settings are stored in localStorage/sessionStorage.

## Interface

| Request | Purpose / protection |
| --- | --- |
| GET `/`, `/style.css`, `/app.js` | Embedded public page assets; no configuration or secrets |
| POST `/api/login` | Exact Origin, bounded JSON password, throttled verification |
| GET `/api/status` | Authenticated snapshot; supplies in-memory CSRF token |
| GET `/api/events` | Authenticated bounded decision history |
| POST `/api/action` | Authenticated session, exact Origin and CSRF; narrow explicit operations |

Mutations are `off`, `auto`, `timed_on`, `check_now`, `settings`, `logout` only.
There are no arbitrary GPIO, file, shell or URL endpoints. CSP allows same-origin
scripts/styles without inline execution; responses are no-store and cannot be
framed. The local LAN, browser/OS and physical USB are part of the trust boundary.

## Reproducible browser verification

The browser suite serves the production assets against explicitly **synthetic**
responses on a temporary loopback port. It neither contacts Tesla nor a controller,
and cannot energize hardware. Python 3, Node.js 20+ and pinned Playwright 1.58.2 are
needed only on the test laptop/CI, never on the ESP32:

```sh
npm ci --prefix tests/browser
cd tests/browser
npx playwright install --with-deps chromium
npm test
```

On this Ubuntu 26.04 host, Playwright 1.58.2 needs
`PLAYWRIGHT_HOST_PLATFORM_OVERRIDE=ubuntu24.04-x64` for its supported Chromium build;
the browser was actually launched and tested with that build. CI uses Ubuntu 24.04.
Set `TSL_SCREENSHOT_DIR` to an absolute output directory for desktop/mobile images.
The suite owns and closes its fixture listener and browser, including on failure.

Covered flows include invalid login, commissioning restrictions, confirmations,
OFF superseding a delayed snapshot, settings limits/blank values/dirty drafts,
redacted export, hostile text, event fetch failure, fault-state sign-out, session
expiry, local-only countdowns, and desktop/mobile overflow checks. These are UI
checks; native tests exercise production policy/client/storage logic, and actual
ESP32 HTTPS/USB checks remain separate from the synthetic server.

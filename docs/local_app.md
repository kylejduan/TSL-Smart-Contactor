# Local application

Open the controller's configured **HTTPS** LAN address. All page assets are served
by the ESP32; there are no CDNs, map providers, external fonts, telemetry scripts,
service workers or browser credential storage. The per-device certificate and
local CA are generated during USB preparation. Port 80 is not served.

Administrator passwords use the same 16–128 UTF-8-byte limit during USB setup and
browser login. ASCII characters use one byte each; accented characters and emoji
can use more. The browser checks the encoded length before sending a login request.
This does not change the password hashing or authentication rate limits.

## Reading the dashboard

- **ON commanded / OFF commanded** is the last recorded GPIO command, never proof
  of receptacle voltage or charging.
- **AUTO permission** is the independent HOME decision from the selected position
  policy. `expire` requires a live lease; `hold_last` may retain it after that lease
  elapses. Manual override and dry-run do not create this permission.
- **Reported position** is the distance from returned coordinates to configured
  home, labeled freshness unverified. Distance alone cannot authorize AUTO.
- **Location evidence** identifies the selected basis. Strict `gps_source` requires
  GPS acquisition time; missing or unusable time reports
  `gps_source_time_unusable`. Opt-in `vehicle_report` instead validates vehicle
  report time and shows report age; it does not establish GPS acquisition age.
  Malformed JSON or coordinates remain `malformed_data`. Raw GPS timestamp values
  remain available in diagnostics even when report mode is selected.
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
Check-now uses the same scheduler and has a 10-minute cooldown. The Diagnostics
panel shows reserved live-data attempts against the fixed 5,000/month limit and
their cost at Tesla's published rate, before the account discount.

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

The position basis defaults to **GPS source time** (`gps_source`), including for
existing configurations. Its 120-second maximum age applies to
`drive_state.gps_as_of` and preserves the original source-age rule.

**Latest reported position** (`vehicle_report`) is a deliberate alternative. It
requires an ONLINE status check, valid coordinates and a valid
`drive_state.timestamp` in whole milliseconds. The same 120-second age and
30-second future limits apply to that report, whose GPS coordinates may be older.
The ordinary 900-second lease and fixed 24-hour sleeping-home ceiling are anchored
to the last qualifying report timestamp; repeated or older reports cannot renew
them. This choice accepts weaker evidence than verified GPS source age. It is not
a conversion or repair for a negative `gps_as_of`.

To choose it, review the limitation in Settings, select the desired basis and
confirm the save. The change clears previous evidence/overrides and invalidates
in-flight results; it does not commission the installation or enable physical
output. Neither mode switches itself based on whichever timestamp happens to be
valid, and neither uses local receipt time. The option's live activation awaits
the owner's explicit choice; availability in the interface is not acceptance or
live verification. After a schema 3 configuration is saved, downgrading to older
firmware causes configuration rejection and OFF rather than a silent policy
change. Credentials are not automatically erased.

**Outage behavior** is separate from timestamp selection. `expire` keeps the
bounded lease/sleep rules. `hold_last` retains confirmed HOME through missing data,
Wi-Fi/internet/API errors, OFFLINE, sleep and caps until valid AWAY, explicit OFF,
permanent authorization loss or a critical local fault. It can remain ON indefinitely
while the vehicle is away and departure cannot be confirmed. Lease/sleep numeric
controls are inactive in this mode; acceptance age, polling and spending caps still
apply. The UI shows retained HOME and no outage cutoff rather than calling the old
position fresh. Browser countdowns never issue Tesla requests.

Every reboot starts OFF. Default `expire` requires new HOME. In `hold_last`,
ordinary power recovery restores the durably saved AUTO decision: HOME resumes
after at least 30 seconds OFF even without internet; AWAY, unknown or DISABLED
stays OFF. The page identifies restored historical HOME and pending commits.
Watchdog/panic recovery discards cached permission. A failed/corrupt record inhibits
output. A power cut before OFF/AWAY commits can preserve the previous saved
decision. Timed ON never survives reset or establishes HOME. Saving settings
clears both current and saved decisions and timed overrides, retaining the mode.

The browser may enter dry-run. Arming, leaving dry-run, secret replacement and
network/certificate provisioning remain USB-only. The page includes the exact
helper commands and points to the physical bench checklist. The first successful
login to an older profile takes about 10 seconds while it migrates the password
verifier. Later logins derive the same PBKDF2 key in the browser and are normally
under two seconds on the tested PC. The browser allows 30 seconds and prevents
duplicate submissions. The browser keeps no password or derived key in persistent
storage. Sign-out remains available when a local fault requires recovery.

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


## Firmware updates

The Firmware section supports explicit local signed application uploads. It uses
the same in-memory session, Origin/CSRF checks and command precedence as other
state changes. Every chunk is bounded to 4 KiB. OFF stays available; mode/settings
changes are rejected during installation. The signed application is verified
before boot selection. Credentials and AUTO settings are not sent to or embedded
in the upload file. See [OTA setup and recovery](ota.md); the first migration is
USB-only and subsequent updates need the installation's signing key.

# TSL Smart Contactor

Standalone, read-only Tesla presence controller for the **Waveshare
ESP32-S3-Relay-1CH, internal antenna, SKU 32152**. The ESP32 calls Tesla Fleet API
over Wi-Fi and explicitly commands GPIO47 HIGH/LOW. A separate control task keeps
OFF and authorization deadlines independent of network requests.

Source, pinned ESP32-S3 builds and offline tests are provided. The program ships
**uncommissioned, DISABLED and dry-run**. Each installation must complete its own
[USB-only bench checklist](docs/bench_checklist.md) before output is enabled.

See [verification](docs/verification.md) for software checks and measurement
limits, [acceptance](docs/acceptance.md) for requirements, and
[contributing](CONTRIBUTING.md) for public/private data handling.

## What it controls

The continuously powered 5 V controller's normally-open relay requests closure of
the existing Finder 22.32.0.230.4340 contactor coil. The intended installation uses
a Mean Well HDR-15-5 and the contactor's two poles for a NEMA 6-20R, 240 V/16 A
vehicle load on a 20 A branch. This project does not redesign or approve that mains
installation. Protection, ratings, wiring and installation approval require their
own electrical assessment. Firmware is not an emergency disconnect. There is no
current/voltage/auxiliary-contact sensing: **ON commanded / OFF commanded** are
software commands, never voltage or charging confirmation. Welded contacts and
what is plugged into the outlet cannot be detected.

No runtime laptop, external server, Home Assistant, MQTT, Tesla middleware, wake
command or vehicle command is used. A laptop and static HTTPS application-domain
hosting are needed for Tesla onboarding; keep the registered public key hosted.

## Build and offline tests (Linux)

Use Git, CMake ≥3.22, Ninja, a C/C++ compiler and Python 3.12. On Debian/Ubuntu,
install the ESP-IDF prerequisites (`git wget flex bison gperf python3 python3-venv
cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0`). Allow roughly
8 GB for the SDK/toolchain. Use a disk with sufficient free space.

```sh
./tools/bootstrap.sh
export IDF_TOOLS_PATH="$PWD/.tools/toolchain"
. .tools/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py size

python3 -m venv .venv
.venv/bin/python -m pip install -r tools/requirements.txt
cmake -S tests -B build-host -G Ninja
cmake --build build-host
ctest --test-dir build-host --output-on-failure
.venv/bin/python -m unittest discover -s tests -p 'test_*.py' -v
```

ESP-IDF is pinned to **v5.5.2** and its release commit/submodules. Native tests are
CMake tests of the actual production C++ sources; there is only one firmware build
system. Host address/undefined-behavior sanitizers are enabled by default.
Reproducible-build mode omits build timestamps and maps source paths. The firmware
uses the S3R8's documented 8 MB octal PSRAM for TLS; a missing/undersized RAM check
inhibits output. The Python helper pins `cryptography==46.0.3`, `pyserial==3.5`, and their tested transitive dependencies
in `tools/requirements.txt`.

`build/tsl_smart_contactor.bin` is the application, not a standalone full-flash
image. Builds are unsigned; sign the application with your installation key before
deployment. Firmware 0.2.0 provides local authenticated, signed OTA with two app
slots and startup rollback. Existing factory-layout installations need one
USB migration; follow [OTA signing, migration and recovery](docs/ota.md). Preserve
NVS and never use `erase-flash` as an automatic troubleshooting step. Do not flash
an unsigned image if you expect OTA availability.

## Windows laptop

Install [Espressif's ESP-IDF v5.5.2 Windows tools](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/windows-setup.html)
and run `idf.py set-target esp32s3`, `idf.py build` from its command prompt in this
repository. Do not use Arduino or PlatformIO settings. In PowerShell with Python
3.12 installed:

```powershell
py -3.12 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r tools\requirements.txt
.\.venv\Scripts\python.exe tools\onboard.py prepare
.\.venv\Scripts\python.exe tools\onboard.py run --port COM5
```

Replace COM5 with the actual native USB Serial/JTAG port in Device Manager. Linux
uses the actual `/dev/ttyACM…` port; give your user serial-port permission as needed.
The helper does not flash devices or automatically retry a token handoff. No secret
is accepted through a command-line flag. Run it from an interactive terminal.
Native Windows USB hello/status and firmware flashing have been tested on a board.
Credential provisioning and live onboarding passed; fresh GPS acceptance and
physical commissioning remain separate verification steps.

The current helper requires **USB protocol 2** for provisioning; update the helper
and firmware together. Before AUTO, timed ON, commissioning, output enablement or
Wi-Fi recovery, it reads the device's current command generation before asking for
the password. An intervening OFF/configuration change makes the pending command
fail instead of applying it to a newer state. Refresh/review status and start a new
command after a `stale_command` response. OFF and read-only status remain available
without a generation. A rejected or missing action acknowledgement exits the
helper with a nonzero status; it never reports a rejected OFF as success.

Before provisioning, list nearby **2.4 GHz** networks with
`python tools/onboard.py usb --port COM5 wifi_scan` (substitute the actual port).
This USB-only passive scan needs no credentials and never joins a network. It
returns up to 16 access points, strongest first, with SSID, RSSI in dBm, channel
and the ESP-IDF authentication-mode number. Duplicate names can represent multiple
access points; an empty name can represent a hidden network. Raw SSID bytes are
also hex encoded, and terminal control characters are escaped. The scan waits at
most eight seconds for completion and is limited to one attempt per 15 seconds.
It is rejected once a profile exists, during provisioning, or on a local fault;
it does not interrupt a configured device's Wi-Fi. The ESP32-S3 cannot use 5/6 GHz.

If provisioning loses its acknowledgement, do not resend tokens automatically.
Use `python tools/onboard.py usb --port COM5 status` and then `diagnostics`. The
latter reports only record presence/integrity, the incomplete-provisioning marker,
token usability, the last provisioning stage, reset reason and memory headroom.
It also reports the first local fault source, maximum control-loop gap and Wi-Fi
station MAC/assigned IP, gateway/DNS, and SNTP/UTC readiness. Match that MAC in the
router's DHCP reservation. The board
advertises DHCP hostname `smart-contactor`; its certificate must match the actual
reserved address used in the browser. The hostname alone does not configure DNS.
It never returns the profile or tokens. An incomplete marker inhibits output across
reboot. Preserve NVS; complete an explicit new handoff if recovery needs new consent.
Once regional registration succeeded, answer **no** to repeating registration.

## One-time Tesla/domain setup

Set up public DNS and HTTPS **before** submitting the application's domain fields.
The [Vercel hosting guide](docs/hosting.md) and `hosting/` static site can be deployed
before you have a Tesla client ID. Avoid `tesla` in the application hostname.

1. Create/verify your Tesla developer account and application using the
   [official onboarding procedure](https://developer.tesla.com/docs/fleet-api/getting-started/what-is-fleet-api).
   Administrative approval/domain access remain owner-operated steps. Request
   `openid offline_access vehicle_device_data vehicle_location` only; no vehicle
   command or charging scopes. Choose NA unless your vehicle uses the documented
   EU region. Check payment setup and account spending limit yourself.
2. Configure your application's allowed origin/domain and an **exact registered
   static HTTPS redirect URL**, for example your own `/callback.html`. Do not
   assume localhost HTTP is accepted. This is separate from the device's LAN HTTPS
   hostname. Reserve that LAN hostname/IP in your router/local DNS; no mDNS service
   is included.
3. Run `.venv/bin/python tools/onboard.py prepare` (Windows equivalent above).
   It asks for the domain, exact redirect, device LAN hostname/IP and client ID.
   It creates an offline P-256 Tesla keypair, a private local CA, and a per-device
   TLS certificate in ignored `provisioning/`. Existing keys are never overwritten.
4. Publish **only the contents of `provisioning/public/`** on the application domain.
   Put its callback page at your registered redirect path and the public key at
   `/.well-known/appspecific/com.tesla.3p.public-key.pem`. Verify both via HTTPS.
   Never upload `private/`, `setup.json`, the local CA private key or device private
   key. The callback runs no scripts/analytics and uses no external resources.
   Configure the host to avoid retaining callback query strings, use no analytics
   or access-log export, and send `Cache-Control: no-store` for this page. OAuth codes
   in URLs can otherwise remain in browser history and hosting logs.
5. Import `provisioning/local-ca.pem` into the **local device/browser trust store**
   you will use. Compare the certificate fingerprint locally. The certificate SAN
   must match the device URL exactly. Do not train the browser to bypass certificate
   warnings. Keep the CA private key offline; only the device leaf private key is
   provisioned. The leaf certificate lasts 825 days; renew/reprovision before expiry.
6. With contactor/mains wiring physically disconnected, identify the chip/flash and
   flash the firmware **only when you explicitly choose to do so**:

   ```sh
   # These operate on connected hardware; substitute your actual port.
   python -m esptool --chip esp32s3 --port /dev/ttyACM0 flash_id
   # Prepare your signed bundle as described in docs/ota.md. NEW installations only:
   python -m esptool --chip esp32s3 --port /dev/ttyACM0 write_flash \
     --flash_mode dio --flash_size 8MB --flash_freq 40m \
     0x0 .tools/ota-migration/bootloader.bin \
     0x8000 .tools/ota-migration/partition-table.bin \
     0x21000 .tools/ota-migration/otadata.bin \
     0x30000 .tools/ota-migration/application.bin
   ```

   For an existing provisioned factory-layout device, use `ota_migrate.py` instead
   so the tool backs up and checks NVS preservation. Do not use this initial-flash
   command on a device already using OTA. Check actual flash size ≥8 MB. The schematic and reused settings image disagree;
   see [verified interfaces](docs/verified_interfaces.md). Enter ROM download mode
   with the board's BOOT/RESET procedure if needed. Updates may reset the board;
   disconnect the load for bench work. No eFuse or full-flash erase command is used.
7. Run `.venv/bin/python tools/onboard.py run --port /dev/ttyACM0`. It asks before
   acting, optionally registers the domain in the selected region, opens a printed
   Tesla consent link, validates the manually copied HTTPS callback URL/state,
   exchanges the code locally, lists VINs and requires you to type one exact VIN.
   It then asks for home coordinates, Wi-Fi credentials and a ≥16-character local
   administrator password. No address/VIN/home is inferred.
8. The helper sends the refresh token/config/per-device TLS credentials over USB,
   waits for a durable-commit acknowledgement, then the device reboots inhibited.
   **The ESP32 is now the sole refresh owner.** The helper never refreshes that
   chain. Do not use the same refresh token in another client. Close the helper;
   Python cannot guarantee that all copies in laptop RAM have been erased.

Ordinary NVS is plaintext storage. Firmware suppresses SDK logs, core dumps and
panic dumps, but physical flash/debug access can recover secrets. No secure-boot,
flash-encryption or irreversible provisioning is performed. Keep the laptop's
private folder protected and never paste credentials into issues or chat.

If an already provisioned board stops joining Wi-Fi, inspect `usb diagnostics`
before changing the router. `wifi_disconnect_reason` is the ESP-IDF reason code;
202 is authentication failure and 205 is a general connection failure. The USB
diagnostic also reports connection attempts and the last `esp_wifi_connect()`
error code, without exposing the stored SSID or password. The firmware enables
both supported WPA3 SAE derivation methods, but a reason code by itself does not
prove whether the password, AP security settings or AP behavior is responsible.
After selecting persistent DISABLED (use authenticated USB `off` if Wi-Fi is down),
update **only** the Wi-Fi credentials without repeating Tesla consent or touching
the refresh-token chain. For an assembled installation, first isolate the branch
circuit and verify it is de-energized before connecting USB:

```sh
.venv/bin/python tools/onboard.py usb --port /dev/ttyACM0 wifi_update
```

On this Windows/WSL bench, `tools/windows_usb_wifi_recovery.ps1 -Port COM8` uses
the local repo toolkit when present. It prompts without echo for the local
administrator password and current 2.4 GHz SSID/password. Keep the branch circuit
isolated and verified de-energized. The operation durably replaces one profile
record and reboots DISABLED. It preserves the prior commissioned and dry-run
settings, but never resumes AUTO on reboot. The firmware rejects a Wi-Fi update
unless DISABLED was already persisted before this request. Verify USB status and
Wi-Fi diagnostics after reboot; select AUTO separately when appropriate.

## Commissioning and everyday use

Follow [the bench checklist](docs/bench_checklist.md) before these acknowledgements.
To measure actual ON polarity before arming, use the separately built
[isolated USB relay diagnostic](docs/relay_bench.md), with explicit permission for
its temporary image and two-second pulse. It restores the existing production
application; it is not an unarmed ON endpoint in production firmware.
They are explicit local actions, not automated build steps:

```sh
.venv/bin/python tools/onboard.py usb --port /dev/ttyACM0 status
.venv/bin/python tools/onboard.py usb --port /dev/ttyACM0 arm
# Still DISABLED and dry-run. Test policy and diagnostics first.
.venv/bin/python tools/onboard.py usb --port /dev/ttyACM0 auto
# Only after bench checks, explicitly enable physical relay commands:
.venv/bin/python tools/onboard.py usb --port /dev/ttyACM0 enable_output
# enable_output returns to DISABLED; choose AUTO again deliberately.
```

Open `https://<the configured LAN hostname or IP>` and sign in. The page provides
AUTO, persistent OFF/DISABLED, a timed ON override (one-hour default, eight-hour
maximum), check-now, diagnostics and validated settings. Every state change needs
an authenticated POST, exact Origin and CSRF token. A session lasts 15 minutes;
login throttling rises after failures. No public router ports should be opened.

The responsive dashboard distinguishes AUTO permission, actual GPIO command,
reported distance and evidence accepted under the selected position policy. It includes the last 16 RAM control
decisions, controller health, request usage, inline setting limits, and USB recovery
guidance. A diagnostic download uses an explicit allowlist and omits VIN, home/GPS
coordinates, client ID, passwords, cookies and tokens. Review it before sharing.

For overnight or departure review, the optional
[local observation recorder](docs/observation.md) saves status and decision history
on a computer. It does not make Tesla requests or control the relay. The computer
must remain awake; the controller's own 16-entry RAM history does not survive reboot.

Status is an explicitly refreshed snapshot. Local countdowns do not generate
network requests; neither page loading nor refreshing status queries Tesla.
Unsaved setting edits survive status refresh. Settings changes require confirmation,
clear prior leases/overrides, and retain the selected AUTO/DISABLED configuration.
OFF stays available during other pending requests and in confirmation dialogs.
Timed ON is unavailable until commissioned and AUTO is selected. Check-now is
rejected while DISABLED, without Wi-Fi/UTC, during a request or before backoff.
The UI and server both enforce their respective restrictions; the device remains
responsible for authorization. See [local app guide](docs/local_app.md).

Select AUTO before requesting a timed override: DISABLED blocks overrides. A timed
override conspicuously bypasses Tesla presence/connectivity, but does not create
HOME evidence. OFF cancels it. On expiry the controller uses independently maintained
AUTO permission, preserving ON without a pulse when valid. Reboot loses all leases
and overrides. Default `expire` AUTO must acquire **new** qualifying evidence under
its selected position policy before resuming; explicitly selected `hold_last` can
restore its durably saved AUTO decision. Every boot/OFF requires 30 seconds
continuously OFF before ON.

USB recovery OFF is also available:

```sh
.venv/bin/python tools/onboard.py usb --port /dev/ttyACM0 off
```

## Presence, latency and cost

The default position policy is `gps_source`, preserving the original requirement
for GPS acquisition time from `drive_state.gps_as_of`. Its defaults are 100 m enable
radius, 200 m disable radius, 120-second maximum GPS age,
30-second future tolerance, 900-second lease, 24-hour maximum sleep extension from
the last qualifying GPS source, and 600-second polling. The space between radii
retains a still-valid prior latch. A sleeping car cannot establish HOME after boot
or an expired lease. ONLINE/OFFLINE/network errors alone never renew anything.

An owner may explicitly select `vehicle_report` in authenticated Settings to use
**latest reported position** instead. This accepts coordinates only after a
successful ONLINE status check and a valid `drive_state.timestamp` in whole
milliseconds. The 120-second age limit then applies to the **vehicle report**, not
to GPS acquisition. The coordinates may be older than the report. This option does
not preserve the original 120-second GPS-age guarantee and does not fix or convert
negative `gps_as_of` values; those remain visible in diagnostics.

In report mode, the same 900-second lease and fixed 24-hour sleeping-home ceiling
are anchored to the last qualifying **report** timestamp. Duplicate, older,
missing, stale or invalid report timestamps cannot renew permission. The selected
basis is fixed until deliberately changed; neither mode falls back to receipt time
or automatically chooses the other timestamp. Saving a basis change clears all
old evidence and overrides and rejects outstanding results from the old setting.
Existing configurations and new provisioning default to strict `gps_source`.
Review this tradeoff before changing `position_basis`; see the
[local application guide](docs/local_app.md#settings-and-recovery).

The independent **Outage behavior** setting defaults to `expire`. Select
`hold_last` to retain the last confirmed AUTO HOME/AWAY decision when there is no
new valid position. HOME then remains authorized through Wi-Fi/internet/API
failures, malformed or stale data, OFFLINE, sleep, and request-cap exhaustion.
The ordinary freshness lease and 24-hour sleep ceiling no longer cause shutoff;
the dashboard marks HOME as **retained** when its freshness lease has elapsed.
A valid newer AWAY report, explicit OFF, permanent authorization loss, or a
critical local fault still clears it. TIMED_ON remains a separate expiring override
and cannot establish retained HOME. Freshness/identity checks still govern new
positions; retries, duplicate data and receipt time do not create new evidence.

**Hold-last can keep the outlet enabled indefinitely while the vehicle is away
and the service cannot confirm departure.** It gives up the bounded outage cutoff
in exchange for charging continuity. Polling/backoff and the fixed $10 gross Data
cap are unchanged; hitting a cap pauses queries rather than spending more.
AUTO/DISABLED and the outage selection survive reboot. In `hold_last`, the
confirmed AUTO decision is also saved separately: ordinary power recovery starts
OFF, then saved HOME resumes after at least 30 seconds OFF, even without internet
or UTC. Saved AWAY, unknown state or DISABLED stays OFF. This restores a historical
decision, never GPS freshness or a timed override. Watchdog/panic resets discard
saved permission and require new HOME. Settings and mode changes clear old saved
decisions. A new HOME waits for durable commit before physical ON; AWAY/OFF act
immediately. A power cut before the OFF/AWAY commit can leave the previous saved
decision; the dashboard reports pending storage. Corrupt records inhibit output.
Saving `hold_last` uses configuration schema 3 without
changing the NVS profile layout or credentials; older firmware rejects schema 3
and stays OFF rather than silently changing the policy.

With the default `expire` behavior, arrival/departure detection normally takes up
to about **10 minutes plus network latency**; retries/caps/asleep state can delay or prevent arrival authorization.
A known fresh AWAY result turns AUTO OFF immediately; otherwise a failed poll leaves
only the existing lease, usually at most 15 minutes from its selected evidence
timestamp. Historical GPS, report-time acceptance and the bounded sleep extension
cannot prove present physical proximity.

Tesla status is checked before live location. The firmware never wakes the car,
but an online live-data call may affect its normal activity and show Tesla's
location-sharing indicator. No guarantee is made that querying an online vehicle
has zero effect on sleep behavior.

Published pricing at review was $0.002 per live data request, with status/list and
auth uncharged and a **$10 monthly account discount** for individual developers/small
applications. An always-online vehicle polled every 10 minutes yields about **$8.64
per 30 days** or **$8.93 per 31 days** before discounts. See the cited
[pricing findings](docs/verified_interfaces.md#pricing-and-limits). The discount
can be consumed by other applications on the account; Tesla's portal billing
limit and usage remain authoritative.

The controller reserves at most **5,000 live-data attempts per UTC month**
(5,000 × $0.002 = $10 at the published rate). This fixed ceiling includes failed
attempts and unused reservations after reboot. Configurable caps still default to
400 total attempts/day and 12,000/month across status, location and refresh.
Check-now has a 10-minute cooldown; automatic polling stays at 10 minutes so a
successful qualifying HOME observation can renew its 15-minute lease. Reaching
any cap pauses polling and renewal. Default `expire` turns OFF at its existing
deadline; explicit `hold_last` retains the last confirmed AUTO decision until a
valid change or local inhibition, without raising the cap. The UI shows conservative reservations
and cost before discounts; other account usage and pricing changes can still lead
to charges.

The optional [ten-minute presence profile](docs/architecture.md#optional-ten-minute-profile)
uses latest reported position, a 600-second report-anchored lease and
540-second polling. Report time is not GPS acquisition time: fresh reports can
contain older coordinates, so this explicitly selected profile cannot guarantee
ten-minute cutoff after physical departure. Duplicate reports and failures do
not renew authorization; valid reported AWAY clears AUTO immediately.
An always-online 31-day month costs approximately $9.92 at the reviewed Data rate,
leaving little room for retries/manual checks under the retained local $10 cap.
See [Tesla pricing](https://developer.tesla.com/) and the
[$10 monthly discount](https://developer.tesla.com/docs/fleet-api/billing-and-limits).

## Recovery and troubleshooting

See the [autonomy audit](docs/autonomy_readiness.md) for the recovery matrix,
prepared fixes and remaining maintenance/physical-test limits. Ordinary outages
can recover automatically in AUTO, but default `expire` after reboot or lease expiry needs
fresh location evidence; a sleeping vehicle cannot restore authorization.

| Symptom | Action |
| --- | --- |
| Always OFF | Check commissioning, dry-run, DISABLED, 30 s dwell, UTC synchronization and timestamp validity for the deliberately selected position basis. GPIO OFF does not prove the outlet is de-energized. |
| `reauthorization_required` | Disconnect mains for USB provisioning and run the helper's consent/handoff again. It resets commissioning/dry-run/mode. Do not replay an old laptop token chain. |
| Lost USB acknowledgement | Inspect `usb … status` first. Do not automatically resend tokens or assume failure/success. Repeating explicit provisioning always disables and requires commissioning again. |
| Interrupted token rotation | Device attempts bounded recovery using Tesla's documented recent-token reuse window. After 32 ambiguous attempts or 24 h from the first uncertain attempt, fresh consent is required. A proven connection failure before any HTTP write restores only that unsent intent; it does not consume rotation recovery or erase earlier uncertainty. |
| `missing_permission` / `billing` / authentication | Permission/authentication problems need owner action. A billing pause automatically retries at the next synchronized UTC month, retaining caps/backoff; payment issues may still need portal action. Authenticated check-now can retry after backoff. Permanent revocation still requires USB. |
| Request cap / rate limit | Review counters and wait for the period/backoff, or deliberately change caps. Browser refresh does not query Tesla. |
| `malformed_data` / unusable evidence time | Check the Tesla response endpoint, HTTP status and fixed detail in the dashboard or USB diagnostics. Strict mode requires valid `gps_as_of`; report mode requires valid `drive_state.timestamp`. Neither accepts receipt time, and report mode does not establish GPS age. |
| Repeated negative `gps_as_of` | Use the [one-shot independent laptop check](docs/independent_gps_check.md) to separate ESP32 parsing from the original Fleet response. It leaves the board OFF and makes no vehicle commands. |
| Clock/Wi-Fi | Check 2.4 GHz Wi-Fi, DNS and SNTP reachability. Reconnection requires a fresh sync; new evidence/TLS stop after six hours without another sync even if Wi-Fi stays up. USB reports sync age. HTTPS management remains local. There is no unattended setup hotspot. |
| Local certificate warning | Check URL/SAN, local CA trust and expiry. Generate a new local directory/certificate and explicitly reprovision; do not disable verification. |
| Browser connection refused | Enter the full `https://` device URL. The controller serves port 443 only; port 80 has no HTTP service or redirect. Use USB diagnostics to confirm its current IP. |
| Local login timeout | Existing profiles take about nine seconds on their first successful login while upgrading the verifier. Later browser logins took 0.7–0.8 seconds for the HTTPS request on the tested Windows client; slower clients may take longer. USB password checks still run the KDF on the ESP32 and need up to 30 seconds. Do not retry rapidly. |
| Storage fault/pending provisioning | Output is inhibited. Recover over USB without erasing flash automatically; interrupted multi-record provisioning must be completed explicitly. |
| Forgotten administrator password | Physical USB replacement provisioning can set a new password. It replaces credentials and resets all arming; physical access is privileged. |

Architecture, exact state behavior and security boundaries are in
[docs/architecture.md](docs/architecture.md). Tests require neither credentials nor
paid requests. Hardware/vehicle/electrical validation remains separate.

The 2026-09-23 physical integration check reached the location endpoint with HTTP
200, but its numeric `gps_as_of` was negative. AUTO correctly remained unauthorized.
The owner repeated the check after driving; the timestamp remained invalid.
Retain the redacted endpoint/status/detail, `gps_source_text`, `fleet_txid`,
`fleet_date`, `fleet_received_utc_s`, `report_timestamp_text` and `api_version` for
Tesla Fleet API support, along with the vehicle software version. These bounded
diagnostics omit tokens, credentials and coordinates. The report timestamp is
**not** a GPS freshness source. The dashboard also shows reported position distance
from configured home, labeled freshness unverified; the distance alone cannot
authorize power. Under strict `gps_source`, unusable GPS time still blocks AUTO.
The opt-in `vehicle_report` policy described above instead accepts report freshness
with the stated limitation; it does not retroactively change these strict-mode
test results. Missing optional metadata is left empty (API version is -1 when
unavailable). Do not infer an offset, wrap a negative number, or substitute receipt
time. Physical output remains inhibited until the separate bench checks and
explicit commissioning/output-enable steps are complete. See
[verification](docs/verification.md).

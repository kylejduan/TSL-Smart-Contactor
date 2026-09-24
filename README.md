# TSL Smart Contactor

Standalone, read-only Tesla presence controller for the **Waveshare
ESP32-S3-Relay-1CH, internal antenna, SKU 32152**. The ESP32 calls Tesla Fleet API
over Wi-Fi and explicitly commands GPIO47 HIGH/LOW. A separate control task keeps
OFF and authorization deadlines independent of network requests.

**Development status:** source, offline tests and ESP32-S3 build are provided.
Firmware is installed over native Windows USB. Wi-Fi, trusted local HTTPS,
onboarding, automatic token rotation and live read-only status/location requests
have passed. AUTO acceptance remains blocked: the live GPS source timestamp is
negative and fails validation. Relay/contact measurements remain unverified. See [verification evidence](docs/verification.md) and the mandatory
[USB-only bench checklist](docs/bench_checklist.md). The [acceptance record](docs/acceptance.md)
maps the original requirements to evidence and open checks. It ships **uncommissioned,
DISABLED and dry-run**. The completed local dashboard revision has passed its
[application checks](docs/app_verification.md); board installation is pending USB
reconnection. Arming and physical-output enablement are separate USB
operations; both leave DISABLED selected.

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
image. Flash through IDF so bootloader and partition table use the correct offsets.
There is no OTA partition or updater. Preserve NVS when updating; never use
`erase-flash` as an automatic troubleshooting step.

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
   idf.py -p /dev/ttyACM0 flash
   ```

   Check actual flash size ≥8 MB. The schematic and reused settings image disagree;
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

## Commissioning and everyday use

Follow [the bench checklist](docs/bench_checklist.md) before these acknowledgements.
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
reported distance and accepted GPS evidence. It includes the last 16 RAM control
decisions, controller health, request usage, inline setting limits, and USB recovery
guidance. A diagnostic download uses an explicit allowlist and omits VIN, home/GPS
coordinates, client ID, passwords, cookies and tokens. Review it before sharing.

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
and overrides. An armed AUTO unit must acquire **new** qualifying GPS evidence
before resuming. Every boot/OFF requires 30 seconds continuously OFF before ON.

USB recovery OFF is also available:

```sh
.venv/bin/python tools/onboard.py usb --port /dev/ttyACM0 off
```

## Presence, latency and cost

Defaults: 100 m enable radius, 200 m disable radius, 120-second maximum GPS age,
30-second future tolerance, 900-second lease, 24-hour maximum sleep extension from
the last qualifying GPS source, and 600-second polling. The space between radii
retains a still-valid prior latch. A sleeping car cannot establish HOME after boot
or an expired lease. ONLINE/OFFLINE/network errors alone never renew anything.

Arrival/departure detection normally takes up to about **10 minutes plus network
latency**; retries/caps/asleep state can delay or prevent arrival authorization.
A known fresh AWAY result turns AUTO OFF immediately; otherwise a failed poll leaves
only the existing lease, usually at most 15 minutes from its source fix. Historical
GPS and the bounded sleep extension cannot prove present physical proximity.

Tesla status is checked before live location. The firmware never wakes the car,
but an online live-data call may affect its normal activity and show Tesla's
location-sharing indicator. No guarantee is made that querying an online vehicle
has zero effect on sleep behavior.

Published pricing at review was $0.002 per live data request, with status/list and
auth uncharged and a $10/month developer discount. An always-online vehicle polled
every 10 minutes yields about **$8.64 per 30 days before discounts**; manual checks,
retries and account-specific conditions change this. See the cited
[pricing findings](docs/verified_interfaces.md#pricing-and-limits). Do not assume
free/unlimited access. Tesla's portal spending limit remains authoritative.

Local caps default to 400 total attempts/day and 12,000/month and include status,
location and refresh attempts. Counts reserve small blocks before use and may
overestimate after reboot. Reaching a cap pauses polling and renewal; it cannot
hold the outlet ON indefinitely. The UI reports location-call cost estimates
before discounts and reservation counts by endpoint in status/location/refresh order.

## Recovery and troubleshooting

| Symptom | Action |
| --- | --- |
| Always OFF | Check commissioning, dry-run, DISABLED, 30 s dwell, UTC synchronization and actual GPS source freshness. GPIO OFF does not prove the outlet is de-energized. |
| `reauthorization_required` | Disconnect mains for USB provisioning and run the helper's consent/handoff again. It resets commissioning/dry-run/mode. Do not replay an old laptop token chain. |
| Lost USB acknowledgement | Inspect `usb … status` first. Do not automatically resend tokens or assume failure/success. Repeating explicit provisioning always disables and requires commissioning again. |
| Interrupted token rotation | Device attempts bounded recovery using Tesla's documented recent-token reuse window. After three ambiguous attempts or 24 h, fresh consent is required. No infinite retry or token erasure occurs. |
| `missing_permission` / `billing` / authentication | Fix owner consent or portal billing manually; authenticated check-now can retry after backoff. Permanent revocation still requires USB. |
| Request cap / rate limit | Review counters and wait for the period/backoff, or deliberately change caps. Browser refresh does not query Tesla. |
| `malformed_data` | Check the Tesla response endpoint, HTTP status and fixed detail in the dashboard or USB diagnostics. Missing/null `gps_as_of` cannot authorize AUTO; do not substitute receipt time or the general response timestamp. |
| Clock/Wi-Fi | Check 2.4 GHz Wi-Fi, DNS and SNTP reachability. HTTPS management remains local. There is no unattended setup hotspot. |
| Local certificate warning | Check URL/SAN, local CA trust and expiry. Generate a new local directory/certificate and explicitly reprovision; do not disable verification. |
| Browser connection refused | Enter the full `https://` device URL. The controller serves port 443 only; port 80 has no HTTP service or redirect. Use USB diagnostics to confirm its current IP. |
| Local login timeout | Password verification took about nine seconds on the bench. Allow 30 seconds for login and authenticated USB commands; do not reduce password hashing strength or retry rapidly. |
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
from configured home, labeled freshness unverified; this can help compare the
returned position with where the owner knows the vehicle is parked, but cannot
authorize power. Missing optional metadata is left empty (API
version is -1 when unavailable). Do not infer an offset, wrap a negative number, or replace source time
with receipt time. Leave DISABLED/uncommissioned/dry-run until valid GPS evidence
and the separate physical bench checks pass. See [verification](docs/verification.md).

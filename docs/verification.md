# Software verification

Public verification records contain reproducible commands, synthetic test
coverage, SDK/dependency versions and limitations. Installation logs, exact trip
or charging times, VINs, coordinates, addresses, MACs and transaction identifiers
must stay in ignored local storage. Never publish a live status export unchanged.

## Reproducible offline checks

```sh
cmake -S tests -B build-host -G Ninja
cmake --build build-host
ctest --test-dir build-host --output-on-failure
.venv/bin/python -m unittest discover -s tests -p 'test_*.py' -v
node --check main/app.js
python3 tools/check_public_tree.py --history
cd tests/browser
npm ci
npx playwright install --with-deps chromium
npm test
```

The native suite links production policy, parser, HTTP decoder, Fleet client,
refresh journal, budget and session code. ASan/UBSan are enabled by default.
Coverage includes boot/inhibition, HOME/AWAY hysteresis, source-based leases,
duplicate/order/generation rejection, clock jumps, sleeping-home ceilings,
override/dwell behavior, HTTP framing/errors, refresh recovery, corruption,
secret redaction and configuration validation. Separate relay diagnostic tests
cover one-shot operation and host restoration. Python tests cover provisioning,
OAuth redirect/state validation, recovery and bounded local observation. Browser
scenarios use synthetic responses and never issue Tesla requests.

On Linux/WSL with GCC or Clang, CTest also compiles the unchanged production
`main/app.cpp`, decision-storage worker and board adapter against test-only SDK declarations. Thirty-seven
separate-process scenarios exercise boot/inhibition, startup failures, failed
queue creation and overflow, GPIO errors, blocked-I/O lease expiry, immediate OFF
with late requests, control deadline and watchdog-feed failures, and recoverable
allocation errors. GPIO commands and state publication are checked before every
watchdog feed. A failed queue initialization must stay faulted/OFF without passing
a null handle into the SDK. These tests use a synthetic clock, GPIO and storage;
they do not model FreeRTOS concurrency or measure actual watchdog reset timing,
radio behavior, contacts or flash writes. The test-only SDK headers never enter
the ESP32 build.

The integration endurance scenario runs 366 synthetic days with recurring Wi-Fi
and API outages, daily HOME/AWAY, sleeping intervals, refresh rotation and month
rollover. Report-profile cases separately test fixed sleep ceilings and spending
caps. All fixtures are synthetic; no paid requests or real credentials are needed.

## ESP32-S3 build

```sh
./tools/bootstrap.sh
export IDF_TOOLS_PATH="$PWD/.tools/toolchain"
. .tools/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py size
```

The released SDK is ESP-IDF v5.5.2 with its pinned commit/submodules and the checked
HTTPS allocation-cleanup patch. CMake rejects an unpatched SDK. Python and browser
dependencies are pinned in their requirement/lock files. CI builds production
and the separately selected relay bench target; CI never flashes hardware.
The application partition is 3 MiB. Inspect the size for the particular binary
being installed rather than reusing a historical installation hash or size.

## Verification limits

Outage-policy tests exercise the production `Policy` and entrypoint for retained
HOME through blocked/missing I/O, freshness expiry and clock discontinuities,
valid AWAY, local OFF/fault priority, TIMED_ON separation, saved HOME/AWAY restoration,
commit gating/corruption/power loss and watchdog/panic reset inhibition,
legacy padding migration and unchanged budget enforcement. Browser/recorder tests
check explicit opt-in and stale/retained status. These are synthetic tests;
building a new policy does not install it on a running controller.

Passing host tests or a build does not prove contactor state, receptacle voltage,
actual charging, startup behavior before firmware runs, real radio/DHCP behavior,
NVS endurance or recovery from a physical brownout. Keep per-installation results
locally and complete [bench acceptance](bench_checklist.md). No general physical
presence guarantee or indefinite unattended-operation guarantee is claimed.

## Hold-last software verification

The hold-last implementation passes all 39 CTest entries with ASan/UBSan: 127
policy/parser/client/reliability cases, nine bench-protocol cases and 37 production
control-entrypoint scenarios. The 55 Python tests and 15 synthetic browser
scenarios pass. The public-tree history guard and JavaScript syntax/diff checks
pass. The ESP32-S3 target builds with the pinned ESP-IDF v5.5.2; `idf.py size`
reports 1,034,143 bytes of image content (1,034,256 bytes in the padded binary),
within the 3 MiB application partition.
These are software checks, not live outage/power-recovery or electrical tests.
The policy must be explicitly selected after installing compatible firmware.


## Signed OTA software verification (0.2.0)

The application-only OTA implementation passes all 56 CTest entries with
ASan/UBSan: 133 core cases, nine bench-protocol cases, 40 production control
scenarios and 14 executions of the real OTA adapter with fake SDK flash/boot APIs.
The 66 Python tests and 18 synthetic browser scenarios pass. Tests cover bounded
sequential uploads, source/generation cancellation, OFF/dwell behavior, flash
write and ambiguous boot-selection failures, missing profiles, startup timeout,
update-task allocation failure and rollback requests. HTTP browser fixtures model
server responses; they do not substitute for an embedded HTTPS integration test.

Production and isolated bench targets compile with ESP-IDF v5.5.2 and esptool
4.12.0. The production image content is 1,054,583 bytes; SDK secure padding makes
the unsigned build 1,114,112 bytes, and the RSA-signed deployment image is
1,118,208 bytes, fitting either 3 MiB slot. Both builds leave hardware secure boot,
flash encryption and anti-rollback disabled. The SDK TLS cleanup checks still pass.

`python tools/test_signed_firmware.py --build build-ota` verifies signatures and
the exact migration layout using the actual build and ephemeral synthetic keys.
Wrong-key, modified, truncated and unsigned images are rejected. Verification
also checks that the SDK metadata-derived signature offset matches the signed
sector. Injected USB migration tests check protected backups, exact NVS/PHY
preservation, 4 KiB write boundaries and refusal of unsupported layouts/security.
The signing helpers never contact hardware or Tesla. CI runs the additional SDK
signing check after its production build.

These results do not establish hardware OTA readiness. The isolated first USB
migration, real authenticated wireless upload, interrupted transfer, pending-boot
reset/rollback, physical GPIO/COM–NO behavior and measured flash-load scheduling
remain manual checks in [ota.md](ota.md#verification). No firmware was installed,
relay energized, live API query made or billing setting changed by these checks.

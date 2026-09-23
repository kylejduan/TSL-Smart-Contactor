# Verification record

As of **2026-09-23**, before physical commissioning. Automated Tesla inputs were
synthetic; no credentials or paid requests were needed. The initial development
record below is supplemented by the subsequent physical USB check.

## Passed locally

| Check | Result |
| --- | --- |
| `cmake -S tests -B build-host -G Ninja` | Configured with GNU C++ 15.2.0, C++17 |
| `cmake --build build-host` | Passed, warnings treated as errors |
| `ctest --test-dir build-host --output-on-failure` | Passed: one executable containing **46 C++ tests** |
| Native sanitizers | AddressSanitizer + UndefinedBehaviorSanitizer enabled; passed, including 10,000 deterministic parser mutation cases |
| `.venv/bin/python -m unittest discover -s tests -p 'test_*.py' -v` | **10 Python tests passed**, Python 3.12.13 |
| `.venv/bin/python -m pip check` | No broken requirements |
| `node --check` on the embedded script | Passed JavaScript syntax check; not a browser/hardware integration test |
| Python bytecode compilation / helper `--help` | Passed |
| ESP32-S3 `idf.py build` | Passed using pinned **ESP-IDF v5.5.2**, commit `30aaf64524299d3bde422ca9a2848090d1bc5d0f` |
| Xtensa compiler | `xtensa-esp-elf-g++ (crosstool-NG esp-14.2.0_20251107) 14.2.0` |
| `idf.py size` / partition fit | Passed; application fits the 3 MiB factory partition |
| Effective SDK configuration | 8 MB DIO/40 MHz flash, octal 40 MHz PSRAM with TLS external allocation, USB Serial/JTAG, 1 kHz RTOS tick, 3 s task watchdog, silent panic, full TLS roots/time validation, IPv4/one DNS server, reproducible-build mode |
| Whitespace/source review | `git diff --check` passed; only this new repository's files staged |

Host tests exercise the production policy, strict JSON parser, bounded HTTP
framing decoder, Fleet request/refresh sequencing, atomic-store journal logic,
request reservations/scheduler, and session/CSRF comparison code. They cover
source-anchored leases, sleep ceiling, OFF precedence, 401 retry limits, caps,
clock jumps, token rotation failure and redaction. Fake GPIO edge tests verify
30-second dwell and no renewal-induced OFF pulse. Python tests exercise the actual
helper's state/redirect checks, large IDs, synthetic API failures, certificate/key
generation, public/private file separation and a fake serial handoff.

## Build artifact

- Application: `build/tsl_smart_contactor.bin`, **896,464 bytes** (875.45 KiB).
- SHA-256: `b7fea30e878d34eecc5bd024af46280b7e06eef227d0ee730e6675aad0188fb2`.
- Bootloader: 13,744 bytes; partition table: 3,072 bytes.
- Linked image sections: 896,347 bytes before binary padding.
- Static DIRAM usage: 143,351 bytes; linker-reported remaining DIRAM: 198,409 bytes.
  This excludes runtime stack/heap use and is **not** a measured free-heap margin.
  TLS uses the documented 8 MB external PSRAM; control state and task stacks are
  internal. Actual detection, memory pressure and stack high-water marks still
  require the board.

SDK reproducible-build mode removes timestamps and remaps paths; no claim is made
that independently built Linux/Windows artifacts were compared byte-for-byte.
The image is a local build artifact, ignored by Git. Use the repository's IDF
commands to build/flash all required images with matching partition offsets.

The first SDK installation exhausted the then-limited Linux volume. Only this
task's downloaded tools were moved to a volume with available space; incomplete
source writes were restored and the SDK installation/build were rerun successfully.
The build used `IDF_TOOLS_PATH="$PWD/.tools/toolchain"` and a local Python 3.12
virtual environment at `IDF_PYTHON_ENV_PATH="$PWD/.tools/idf-python"`. Neither
machine-local configuration nor downloaded tools are committed.

## Initial development limitations

The following records the boundary at the initial build, before the subsequent
authorized USB work and static-site deployment. See the dated USB update below
and [hosting record](hosting.md) for later checks.

- Physical board SKU/revision, flash ID, PSRAM detection, GPIO polarity, COM–NO
  continuity, startup/ROM/reset/brownout pulses, watchdog recovery or real ≤100 ms
  timing under flash/TLS/login load.
- Live Tesla registration, consent, paid requests, token rotation/revocation,
  per-vehicle `gps_as_of` availability/freshness, API latency or sleep effects.
- Real USB transport on Linux/Windows, physical NVS power loss, LAN certificate
  import, browser-to-device HTTPS/authentication integration or sustained runtime
  memory use. Protocol/security logic tests are not substitutes for those checks.
- Mains installation, protection, contactor operation, actual output voltage,
  charging, welded contacts or identifying a connected load.
- No flash, live provisioning, relay actuation, Tesla wake/vehicle command, account
  billing change, eFuse operation, deployment or service restart was performed.

Follow [the bench checklist](bench_checklist.md) and [README](../README.md) for the
owner-operated next steps. No validation server/listening port was started, and
no background validation process is intentionally left running. SDK/toolchain and
build outputs remain available for reproducibility.

## USB board check 2026-09-23

The owner confirmed USB-only power with all contactor/mains wiring disconnected.
Firmware source at checkout `41605b8` was unchanged from the tested application
artifact above. Native Windows Python 3.12.13, pyserial 3.5 and esptool 4.12.0 were
used, with the production `device_setup.usb_exchange` helper for protocol checks.

| Check | Observed result |
| --- | --- |
| USB/chip detection | Native USB Serial/JTAG; ESP32-S3 QFN56 revision v0.2, 40 MHz crystal, embedded 8 MB PSRAM |
| Flash identification | Manufacturer `20`, device `4018`, detected 16 MB; quad flash at 3.3 V |
| Pre-flash preservation | Full 16,777,216-byte flash backup completed to a private, user-restricted local directory; its contents were not published |
| Firmware write | Bootloader at `0x0`, partition table at `0x8000`, application at `0x30000`; all three esptool hash checks passed |
| Erasure boundary | Only firmware write sectors were erased; no full-chip erase, explicit NVS erase or eFuse write was issued |
| USB hello | Protocol 1, board `ESP32-S3-Relay-1CH`, firmware `0.1.0`, `secrets_echoed:false` |
| Initial USB status | `ready:false`, `commissioned:false`, `disabled:true`, `dry_run:true`, `commanded_on:false`, `fault:false` |
| Additional reset | esptool chip identification followed by RTS hard reset; hello/status returned the same inhibited state |

The application enforces an initialized PSRAM size of at least 8 MB before reporting
no critical fault. This passed its startup guard; sustained TLS memory pressure was
not tested. `ready:false` is expected before credential provisioning. No automatic
NVS erase recovery was needed. The conservative 8 MB firmware layout is unchanged.

These are software/USB observations, **not physical relay measurements**. There was
no request to arm, enable physical output or command ON. COM–NO continuity, actual
GPIO polarity, boot/reset pulses, power removal, watchdog fault injection and
electrical safety remain unverified. Tesla consent/registration, credential handoff,
Wi-Fi, local HTTPS and real token refresh also remain unverified at this checkpoint.
USB handles were closed after each command; no USB monitor was left running.

## USB Wi-Fi scan 2026-09-23

Added an unprovisioned-only passive USB scan using ESP-IDF Wi-Fi APIs. The target
build passed with the same v5.5.2 SDK; the application was 908,992 bytes (SHA-256
`efa9d4c4301fb3598b6a88fac0c4f9a40290169c3fc7f52707a66361222d603f`). Only the
application partition was updated; esptool verified its write hash. The physical
board returned 16 access points and indicated that additional results were omitted.
Network names and addresses are intentionally excluded from this public record.
The 46 native tests and 12 Python tests passed, including synthetic SSID terminal
escaping and no-retry handling. Scan timeout/fault injection remain untested on
hardware. A subsequent credential handoff timed out and the board reported a local
fault; that separate provisioning failure requires diagnosis before commissioning.

## Provisioning recovery 2026-09-23

The owner reported successful regional registration, OAuth consent and vehicle
selection, followed by a missing USB acknowledgement. Subsequent physical USB
status showed `ready:false` and a critical fault with OFF commanded. Read-only
storage diagnostics after an application-only repair showed the incomplete
provisioning marker present, no profile record and no usable token journal.
Existing NVS was backed up privately before repair and was not erased or cleared.

The pre-repair target disassembly showed a 15,200-byte USB dispatch frame and an
8,320-byte nested token-provision frame, before deeper NVS calls, against a
24,576-byte USB stack. This establishes inadequate headroom and is consistent
with the observed interruption at token storage; the original reset reason was
not captured. The serialized USB parser now uses static storage, token journal
save seals its caller's candidate by reference instead of copying another full
record, and the USB stack is 32,768 bytes. The last provisioning stage and stack
watermark are available through metadata-only `usb diagnostics`. The helper now
allows 90 seconds for the complete handoff without automatic retry.

The 46 native tests, including token journal failure/recovery, and 12 Python tests
passed. ESP-IDF v5.5.2 rebuilt the target successfully: application 923,168 bytes,
SHA-256 `c0e40c063b26b930cfd765c499a442cadc829f8a33a1b17d0cb283c578049e22`.
The application-only flash hash check passed. A 14,000-byte synthetic padding
field on a read-only hello request received a valid reply in 0.06 seconds during
repair validation; no synthetic token/profile was installed. After the final
stack-size update, USB status/diagnostics still showed the preserved incomplete
marker and OFF inhibition, with 26,416 bytes of observed USB stack headroom during
these read-only commands. This is not a provisioning/TLS peak measurement.

Fresh owner-operated consent and a completed handoff are still required to verify
the repair end to end. Regional registration need not be repeated. No commissioning,
physical output enablement or ON request was performed during diagnosis.

## Successful handoff and network startup repair 2026-09-23

The owner's fresh consent and USB handoff completed after the provisioning repair.
After reboot, physical USB diagnostics confirmed the profile present with valid
integrity, incomplete marker cleared and a structurally usable token journal.
This confirms durable credential handoff; it does not establish live refresh or
location polling. The controller remained DISABLED, uncommissioned and dry-run.

A separate startup fault was then observed. Added first-fault and control-gap
diagnostics identified `control_deadline` with a 345 ms maximum gap, no failed
allocation, and a working Wi-Fi connection. Pinning `app_main` to core 1 and the
setup task to core 0 (where Wi-Fi runs) removed that fault in the observed boot.
At 58 seconds uptime, Wi-Fi was connected, first-fault was `none`, no allocation
failure was recorded and the maximum control-loop gap was 51 ms. Neither the
250 ms fault threshold nor the watchdog timeout was relaxed. This short bench
observation is not worst-case latency validation or a substitute for load testing.

The firmware now advertises DHCP hostname `smart-contactor` and exposes its station
MAC and assigned IP through USB diagnostics. A router-assigned address differed
from the owner's intended reservation/certificate address; the normal browser URL
therefore remains blocked pending reservation correction. No address was forced
onto the LAN and no certificate/hostname verification was disabled.

Using the assigned IP only as a TCP connection override, with the intended IP
retained as the TLS verification identity and HTTP Host, the local CA validated
the server certificate. GET `/` returned 200 with bytes identical to the embedded
HTML; unauthenticated GET `/api/status` returned 401 and POST `/api/action` with
an empty JSON object returned 403. No session/password, authenticated action,
Tesla request or relay actuation was involved in these checks. Normal browser
access and authenticated login remain separate pending checks.

ESP-IDF v5.5.2 target build passed; application 924,304 bytes, SHA-256
`e0abbdc1017eeb8d40e9358ddb66f46ffab8bbe37458070b5db3af28e2cd29cc`.
Application-only flashing passed the esptool hash check. A private NVS backup was
retained before firmware changes; saved credentials survived. All 46 native and
12 Python tests passed again. Physical relay/contactor operation remains untested.

# Isolated relay polarity measurement

This **project-developed diagnostic**, not a vendor-endorsed procedure, exists
only to measure GPIO47 active-high behavior before production commissioning.
It uses the same pinned ESP-IDF build system and `main/board.hpp`. A separate
build selects only the diagnostic source. The production image has no diagnostic
command, test transport or unarmed ON path.

Keep the controller powered by USB only, with COM/NO disconnected from all
contactor/mains wiring. Observe the isolated COM–NO contacts with a meter. Never
use continuity mode on an energized circuit. Obtain explicit operator permission
before flashing this image or commanding the pulse; general software-development
permission does not authorize a physical ON test.

## Prepare without touching the board

Preserve the exact installed production application and its SHA-256 in an ignored
local directory. The runner verifies that image against installed flash before
writing. Do not use a newly rebuilt production image as the backup unless its
bytes match the installed image. Preserve `build/` and use **`build-bench/`**:

```sh
export IDF_TOOLS_PATH="$PWD/.tools/toolchain"
. .tools/esp-idf/export.sh
idf.py -B build-bench -D SDKCONFIG="$PWD/build-bench/sdkconfig" \
  -D IDF_TARGET=esp32s3 -D TSL_USB_RELAY_BENCH=ON build
sha256sum build-bench/tsl_relay_bench.bin
```

Never use `idf.py flash` for this procedure: the runner writes **only** the
selected running application (legacy factory at `0x30000`, or the checked OTA
slot). Bootloader, partition table, NVS and PHY data
stay outside the written region. On OTA firmware the runner queries USB
`firmware_status`, requires a confirmed/normal signed baseline and no update in
progress, and uses its allowlisted running slot for both diagnostic and restoration.
It verifies the frozen production image at that offset before writing. Both
legacy/OTA application partitions are 3 MiB. The diagnostic
does not initialize Wi-Fi, NVS, Tesla, HTTPS or production configuration.

Run the native diagnostic tests and the host restoration tests before hardware:

```sh
cmake -S tests -B build-host -G Ninja
cmake --build build-host
ctest --test-dir build-host --output-on-failure
.venv/bin/python -m unittest discover -s tests -p 'test_relay_bench.py' -v
```

## One explicitly authorized physical test

Use the platform Python with project dependencies and **esptool 4.12.0**
(`python -m pip install -r tools/requirements.txt esptool==4.12.0`) and the actual
USB port. Windows USB access may use the existing
ignored Windows Python under the WSL repository; no separate Windows project
copy or credentials are needed. Substitute the actual MAC and reviewed hashes:

```sh
python tools/relay_bench.py --port PORT --expected-mac BOARD_MAC \
  --bench-image build-bench/tsl_relay_bench.bin --bench-sha256 BENCH_SHA256 \
  --production-image PRESERVED_PRODUCTION_BIN --production-sha256 PRODUCTION_SHA256 \
  --authorize-isolated-usb-relay-test
```

The runner checks the expected device is DISABLED, uncommissioned, dry-run and
OFF, with intact configuration and a usable token record. It verifies the
production image bytes on flash before installing the diagnostic. Both reviewed
images are copied and rehashed in a private staging directory so another build
cannot replace the restoration bytes mid-test. Any preflight
failure aborts without writing an image.

The diagnostic initializes the inactive output latch before enabling GPIO47 and
starts OFF. After at least 30 seconds continuously OFF, one exact USB START
command containing the current boot nonce may request a **nominal two-second**
pulse. The host prints a five-second warning before sending that command once.
The diagnostic automatically commands OFF at its own monotonic deadline; USB
disconnect cannot renew it. OFF consumes/cancels the single test opportunity.
Early, malformed, duplicate and wrong-nonce START commands cannot energize.
There is no re-arm command; a reboot creates a new nonce and starts OFF again.

The GPIO-owning loop checks deadlines frequently and has a progress watchdog.
This bounds normal software behavior, not the physical contact duration under
every failure. Record actual contact closure while commanded ON and reopening
afterward; a click alone does not verify continuity or polarity. Meter screening
does not exclude a shorter startup/reset pulse.

The runner checks the completed OFF state, restores the production application
in a `finally` block even after a lost acknowledgement or partial flash, verifies
its bytes, and verifies the inhibited production state again. **It never retries
START.** It neither arms production nor enables output, and makes no Tesla calls.
Have the operator report both ON closure and OFF reopening before recording a
passing measurement. Watch for unexpected contact closure during download,
flashing and both image restarts too; record only what was actually observed.

## Interrupted restoration

A host power loss, unplugged USB cable or failed flash can prevent restoration;
software cannot guarantee the `finally` block executes. Leave mains disconnected.
On any runner error, its image staging directory is retained and its location and
expected production SHA-256 are printed. Use that hash-checked production copy
if the original build output has changed. Successful completion removes staging.
The diagnostic boots OFF, but it is not deployment firmware. Reconnect USB and
restore the preserved, hash-checked production image with application-only commands:

```sh
python -m esptool --chip esp32s3 --port PORT --after hard_reset \
  write_flash 0x30000 PRESERVED_PRODUCTION_BIN
python -m esptool --chip esp32s3 --port PORT --after hard_reset \
  verify_flash 0x30000 PRESERVED_PRODUCTION_BIN
python tools/onboard.py usb --port PORT status
python tools/onboard.py usb --port PORT diagnostics
```

Require DISABLED, uncommissioned, dry-run, OFF commanded, no fault and intact
profile/token records. Verify Wi-Fi and UTC recovery. If the initial flash-byte
verification aborted while the board was in ROM mode, a USB power cycle can exit
ROM mode; do not overwrite an unknown image merely to force the test to proceed.
Never erase flash or provision tokens to repair a diagnostic interruption.

After successful restoration, continue `bench_checklist.md`. This measurement
does not itself commission, authorize mains energization or complete all fault,
startup, departure and sleep acceptance checks.

# Local signed firmware updates

Firmware 0.2.1 provides application-only OTA through the controller's local HTTPS
page. There is no update server, background release check, port forwarding, Fleet
request or new paid service. Each installation owns its signing key. Downloaded
open-source binaries cannot be installed until reviewed and signed with that key.

## First USB migration

Older installations have one factory application partition. They need one
explicit USB migration before any wireless update. Disconnect wall power and all
contactor/mains wiring, power only by USB, and select OFF / DISABLED. Do not run
these commands against hardware without approval. Build/package/key commands are
offline; only `ota_migrate.py` changes a device.

Use the pinned ESP-IDF v5.5.2 command environment (with esptool 4.12.0), after
`tools/bootstrap.sh` has applied the project's SDK fix. Signing uses that SDK's
`espsecure` implementation; general provisioning requirements alone do not install
it. Bootstrap installs the signing-tool pin from `tools/requirements-sdk.txt`.
On Windows use the ESP-IDF command prompt in this repository and run
`python -m pip install -r tools/requirements-sdk.txt` before building/signing. If building in
WSL, keep the SDK/toolkit here and use Windows only for its USB connection, or
attach the device deliberately to WSL. There is no separate Windows build-tools
folder. Path arguments below are placeholders relative to the repository.

```sh
# Create this directory ONCE. Keep a protected backup of the key outside Git.
python tools/firmware_sign.py keygen --out .tools/firmware-signing

# A separate SDKCONFIG avoids inheriting pre-OTA settings from an older build.
idf.py -B build-ota -D SDKCONFIG=build-ota/sdkconfig -D IDF_TARGET=esp32s3 build
python tools/firmware_sign.py package --build build-ota \
  --key .tools/firmware-signing/signing-key.pem \
  --public .tools/firmware-signing/signing-public.pem \
  --out .tools/ota-migration

# Hardware step, only after explicit approval and isolation:
python tools/ota_migrate.py --port PORT --bundle .tools/ota-migration \
  --backup .tools/ota-usb-backup
```

In PowerShell put each command on one line or use PowerShell continuation syntax.
Replace `PORT` with the actual USB port. The migration tool requires its exact
local confirmation phrase. It checks USB/ROM device identity, healthy DISABLED
status, hardware security settings and the known legacy partition layout. It
refuses an already migrated device. It verifies the bundle hashes/signature,
saves the configured 8 MiB flash range in a new private directory, writes the
signed application/OTA selection/bootloader/table, reads them back, and checks
that NVS and PHY bytes are unchanged before rebooting. Padding is limited to 4
KiB sectors so partition-table writes cannot cross into NVS. It never erases the
whole flash or burns eFuses. An unexpected power loss during this one-time
bootloader/table migration can require USB recovery; it does not have application
OTA's interruption guarantees.

OFF / DISABLED stays saved. The tool does not change presence settings, arm the
installation or select AUTO. Existing credentials, token journal, counters and
profile remain in their original NVS range. First select AUTO deliberately when
ready; hold-last versus expiry remains the previously selected setting.

After reboot, inspect USB status/diagnostics and sign in locally. The Firmware
section must report the installed release version, `ota_0`, and updater availability. Leave mains isolated
for the first supervised wireless update and rollback/interruption checks.
For USB updater diagnostics use `python tools/onboard.py usb firmware_status --port PORT`.
Measure COM–NO and control scheduling under flash load; a build cannot prove
those timings or the physical output.

## Later wireless application updates

In the pinned SDK environment, build the production target and sign it using the
SAME key. Never sign the relay diagnostic or a full-flash merged image.

```sh
idf.py -B build-ota -D SDKCONFIG=build-ota/sdkconfig build
python tools/firmware_sign.py sign --image build-ota/tsl_smart_contactor.bin \
  --key .tools/firmware-signing/signing-key.pem --out .tools/update-signed.bin
python tools/firmware_sign.py verify --image .tools/update-signed.bin \
  --public .tools/firmware-signing/signing-public.pem
```

Output files are never overwritten; use a new output filename for each release.
Sign in to the local dashboard, choose the signed application `.bin` under
Firmware updates, and confirm Install and restart. The image is sent in bounded,
sequential 4 KiB requests. Keep the page open until it reports acceptance; then
wait about 15 seconds, reload and sign in again. Verify the version, startup
result, mode, reason and ON/OFF commanded. The displayed build fingerprint is the SDK application ELF hash; compare it
with `build_sha256` in the reviewed bundle manifest. It differs from the signed
binary file hash. A previous failed candidate is reported as
`previous_update_failed`; it is historical evidence until that slot is replaced.

The update gate temporarily commands the relay OFF without replacing the saved
AUTO decision or configuration. Polling is paused and an in-flight token refresh
is allowed to complete its durable handoff. TIMED_ON must be canceled before
starting an upload; it is never restored by reboot. OFF remains available and
cancels the upload/boot selection while saving DISABLED. A canceled/failed upload
resumes the current mode after the normal minimum OFF dwell; it does not silently
save DISABLED or manufacture HOME evidence. A completed reboot applies ordinary
startup rules: held HOME may resume after 30 seconds OFF; expiry mode requires new
HOME evidence. Critical faults inhibit output.

## Failure, rollback and trust boundaries

- Wi-Fi loss, page closure, expired session, wrong offsets, missing chunks, oversized
  requests or image/signature failure never select an incomplete image. The
  transfer expires after 30 seconds without progress or 180 seconds overall.
  There is no automatic resend. Read status before retrying.
- Only the inactive app slot is erased/written. After complete verification,
  redundant `otadata` selects it for the next boot. No web route updates the
  bootloader, partition table, NVS or PHY.
- A new OTA boot keeps the relay OFF while checking local storage/profile,
  required PSRAM, task/server creation, GPIO operations, control progress and its
  watchdog for five seconds after setup. Internet/Tesla/GPS are not prerequisites.
  A critical fault or 30-second startup timeout requests rollback. Failure to
  create the update worker requests rollback directly from setup. A crash/reset
  before confirmation lets the bootloader fall back automatically.
- Confirmation checks local startup health; it cannot prove every new feature is
  correct, detect a later regression, or verify contactor voltage. It cannot
  guarantee indefinite autonomous operation or recovery from both damaged slots.
- Rollback changes application selection only. NVS is shared, including the live
  refresh-token chain. Do not restore an old token snapshot. Future releases must
  preserve NVS schema/read compatibility with their rollback predecessor; an
  incompatible schema/layout migration requires a separately reviewed USB path.
- Official ESP-IDF RSA-3072/PSS signed-on-update checks trust the signing digest in
  the running signed application. A session/password alone cannot install an
  image made with a different key. Each modifying request also requires current
  authentication, same-origin/CSRF checks, and the upload ID/order. The project and
  ESP32-S3 descriptor are checked before the first flash write.
- This is software update authentication, **not hardware secure boot**. Hardware
  secure boot, eFuse anti-rollback and flash/NVS encryption remain disabled. An
  attacker with physical flash/USB access can replace firmware or extract ordinary
  NVS secrets. Signed downgrades within the compatible family are not prevented.
- The key never goes into firmware/NVS or Git. Losing it requires deliberate USB
  recovery to establish a new signed baseline. This version does not offer OTA
  key rotation. Protect the laptop and signing-key backup.

USB recovery remains available. Isolate the installation first; retain its
current NVS, identify the selected slot from the partition table/OTA metadata,
and repair application/boot metadata using a reviewed signed image. Do not blindly
write `0x30000` on an OTA device: it may be running `ota_1`. Do not restore the
whole old backup after runtime refresh has resumed. The relay-bench helper queries the running OTA slot before its isolated diagnostic
and verifies/restores the frozen production image at that slot; pending or busy
OTA state is rejected. It never modifies OTA boot metadata.

## Verification

Run native/Python/browser checks from README and the additional SDK signing check:

```sh
python tools/test_signed_firmware.py --build build-ota
```

That check signs the actual build with temporary synthetic RSA keys, verifies the
bundle, and rejects unsigned, modified, truncated and differently signed images.
The real `main/ota.cpp` also runs in native tests with fake SDK flash/boot APIs,
covering OFF during writes/boot selection, write/signature/selection failures,
timeouts and startup confirmation/rollback. USB `firmware_status` provides the same local
update/slot diagnostics for recovery. Those tests do not flash hardware.

Before declaring a particular device OTA-ready, retain a private bench record of
the initial migration, one authenticated signed wireless update, incorrect-key
rejection, interrupted upload, pending-boot reset/rollback, OFF cancellation,
credential/token continuity and measured relay/control behavior. These hardware
checks require explicit approval; they are not automated by the build.

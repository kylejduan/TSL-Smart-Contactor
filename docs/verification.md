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

Passing host tests or a build does not prove contactor state, receptacle voltage,
actual charging, startup behavior before firmware runs, real radio/DHCP behavior,
NVS endurance or recovery from a physical brownout. Keep per-installation results
locally and complete [bench acceptance](bench_checklist.md). No general physical
presence guarantee or indefinite unattended-operation guarantee is claimed.

# Verified interfaces

Reviewed **2026-09-22 to 2026-10-04**. Primary-source findings are separated below from board measurements and live
integration observations. Vendor examples alone do not verify an installed system.
Web documentation is unversioned unless noted. Recheck before changing the parser,
board definition, regions, scopes or pricing.

## Manufacturer facts and unresolved hardware details

| Primary source | Finding |
| --- | --- |
| [Waveshare product documentation](https://docs.waveshare.net/ESP32-S3-Relay-1CH/) | SKU 32152 is the internal-antenna **one-channel**, 5 V board; default MCU ESP32-S3R8. USB is native Type-C. The -U version is a different SKU. |
| [Resources](https://docs.waveshare.net/ESP32-S3-Relay-1CH/Resources-And-Documents/) and [vendor demo ZIP](https://files.waveshare.net/wiki/ESP32-S3-Relay-1CH/ESP32-S3-Relay-1CH-Demo.zip) | `Arduino/examples/MAIN_WIFI_STA/WS_GPIO.h` defines CH1 as GPIO47. `WS_Relay.cpp` writes HIGH for ON, LOW for OFF. Only these hardware facts are reused; demo control services/toggles are not included. |
| [Published schematic, one sheet](https://files.waveshare.com/wiki/ESP32-S3-Relay-1CH/ESP32-S3-Relay-1CH-schematic.pdf) | Pin table maps CH1 to GPIO47. CH1 drives the SS8050/opto/transistor relay circuit active-high; pull-down R18 is shown. USB D−/D+ map to GPIO19/20. MCU is ESP32-S3R8 (octal PSRAM package); flash is labeled **W25Q128JVSI (128 Mbit / 16 MB)**. No RGB LED is selected or used. |
| [Arduino setup page](https://docs.waveshare.net/ESP32-S3-Relay-1CH/Arduino/) | Its settings image shows **8 MB**, QIO/80 MHz, PSRAM disabled, and a filename referring to Relay-6CH. The page also points to generic S3-Zero tutorials. These reused examples are not sufficient evidence of the installed board's flash capacity. |

**Design decision:** use an 8 MB header, conservative DIO/40 MHz flash, 40 MHz octal PSRAM for TLS allocations, and a
3 MB factory application partition. The flash layout fits either cited capacity.
The documented S3R8 has 8 MB PSRAM; runtime checks detection/size and inhibits output
on a mismatch. Control state and task stacks stay in internal RAM. External RAM is
registered only with the capability allocator, not generic malloc. `main/board.hpp` alone owns GPIO47/polarity. USB Serial/JTAG owns
GPIO19/20. RS485, RTC and Bluetooth are not initialized. The SDK owns the octal memory bus
(GPIO26-37); no application peripheral uses those pins. No six-channel pins or RGB
code are imported.

**Outstanding:** inspect assembly revision/antenna variant and measure relay polarity
and all boot/reset intervals. The build still intentionally addresses only 8 MB.
Firmware cannot constrain GPIO before ROM/bootloader/application initialization.

## Tesla authentication and onboarding

| Primary source | Implemented contract |
| --- | --- |
| [Fleet API onboarding](https://developer.tesla.com/docs/fleet-api/getting-started/what-is-fleet-api) | Owner creates an application and hosts an EC P-256 public key at `/.well-known/appspecific/com.tesla.3p.public-key.pem`. Private key stays local. Domain hosting remains necessary; a runtime server does not. |
| [Authentication overview](https://developer.tesla.com/docs/fleet-api/authentication/overview) | Third-party OAuth is the documented path for a hobbyist's own vehicle. Bearer access token authenticates requests. |
| [Third-party tokens](https://developer.tesla.com/docs/fleet-api/authentication/third-party-tokens) | Authorization at `https://auth.tesla.com/oauth2/v3/authorize`; code exchange and refresh at `https://fleet-auth.prd.vn.cloud.tesla.com/oauth2/v3/token`. Code exchange needs the client secret; refresh needs `client_id` and `refresh_token`, not the secret. Refresh tokens rotate; the most recently used token can be reused for up to 24 hours. Published refresh-token expiry is three months. `login_required` requires new consent. |
| [Partner tokens](https://developer.tesla.com/docs/fleet-api/authentication/partner-tokens) and [partner endpoints](https://developer.tesla.com/docs/fleet-api/endpoints/partner-endpoints) | The helper obtains a client-credentials partner token and sends `POST /api/1/partner_accounts` with `{"domain":"…"}`. Registration is per region; application allowed origins/domain must agree. |
| [Regions](https://developer.tesla.com/docs/fleet-api/getting-started/regions-countries) | NA/APAC except China: `https://fleet-api.prd.na.vn.cloud.tesla.com`; Europe/Middle East/Africa: `https://fleet-api.prd.eu.vn.cloud.tesla.com`. China has separate registration/auth requirements and is intentionally unsupported by this version. |

The helper asks for only `openid offline_access vehicle_device_data
vehicle_location` for owner consent. No command, charging or wake scope exists in
the project. Partner registration also requests only read scopes. The secret never
crosses USB. The redirect must be an exact registered static HTTPS URL; the helper
rejects HTTP localhost, mismatched paths, duplicate parameters, fragments and bad
state. No claim is made that Tesla accepts arbitrary redirect URIs.

**Recovery decision:** journal the attempted refresh before sending it; retain
current and previous tokens in one committed blob. Retry an ambiguous rotation at
most 32 times, within 24 hours of its first attempt, with backoff. The 32-attempt
bound is project policy, increased after the 2026-10-05 autonomy audit; it is not
a Tesla requirement. Never try an
unbounded chain of old tokens. An older retained token is forensic/recovery context,
not automatically replayed after `login_required`. A token response is usable only
after replacement-token persistence succeeds. `expires_in` controls proactive
refresh while active. Scope fields, when supplied, must include both read scopes;
otherwise endpoint permission checks still apply.

## Tesla request and response schema

[Vehicle endpoints](https://developer.tesla.com/docs/fleet-api/endpoints/vehicle-endpoints)
were inspected including the expanded client-side schema and response example,
not just the collapsed prose. The page's schema asset on this review was:

`https://developer.tesla.com/docs/8d5ef0369d90cf6456cbf86edc853e380b108a7f-f4a98b02f66ca341980f.js`

| Purpose | Request / response used |
| --- | --- |
| Explicit setup selection | `GET /api/1/vehicles?page=N&per_page=100`; `response[]`, VIN text and pagination. Python integers retain precision, and firmware uses VIN paths, never a rounded numeric ID. |
| Connectivity check | `GET /api/1/vehicles/{vin}`; `response.vin`, `response.state` (`online`, `asleep`, `offline`). Unknown values do not authorize. |
| Live location, only after online | `GET /api/1/vehicles/{vin}/vehicle_data?endpoints=location_data`. `endpoints` is documented as semicolon-separated selectors. |
| GPS fields | Expanded sample returns `response.drive_state.latitude`, `.longitude`, `.gps_as_of`, and `.timestamp`. Coordinates do **not** appear in a `location_data` object. The sample GPS time is `1692137422`, while the general timestamp is `1692141038420`. |

**Conservative interpretation:** `gps_as_of` is GPS source epoch **seconds**, as
indicated by its name/value in the manufacturer's API example. The example does
not provide a formal timestamp-units/accuracy schema or a guarantee that every
vehicle firmware supplies the field. General `drive_state.timestamp` is
milliseconds and can be newer than GPS; strict mode never substitutes it. No accuracy or
GPS-quality field is invented. Missing GPS source time inhibits renewal. Actual
vehicle compatibility/freshness remains a live integration check. Firmware
silently treating a missing source time as current would violate the policy.

Whole-second JSON numbers may use decimal or exponent notation, as allowed by
[RFC 8259 section 6](https://www.rfc-editor.org/rfc/rfc8259#section-6). The parser
accepts those representations only when finite, integral and in the supported
epoch-seconds range; it does not infer milliseconds, parse strings as timestamps,
round fractions or substitute another field. This is a parser design decision,
not evidence that Tesla uses a particular numeric notation on every vehicle.

### Timestamp compatibility boundary

Strict `gps_source` policy requires a valid, recent `drive_state.gps_as_of`.
Negative or otherwise invalid values are rejected. Their meaning and any
age-preserving conversion have not been established; no signedness/epoch/clock
offset correction is applied. Diagnostic captures must remain private.

The optional `vehicle_report` policy instead validates `drive_state.timestamp`
in milliseconds. It deliberately does not prove GPS acquisition age and requires
explicit selection; the firmware never switches automatically. The
[official vehicle protobuf](https://github.com/teslamotors/vehicle-command/blob/main/pkg/protocol/protobuf/vehicle.proto)
declares a separate location-state timestamp and an unsigned `gps_as_of`, but
does not establish the Fleet REST conversion or a source-age guarantee for the
general report timestamp. The endpoint selector is not a response-object name.
A live response can contain cached coordinates. See [architecture](architecture.md#auto-authorization).

## Pricing and limits

The expanded vehicle schema marks list/status `pricing_category: null`, and
`vehicle_data` as `device_data`. [Current public pricing](https://developer.tesla.com/)
lists data at 500 requests/$1 ($0.002 each). [Billing and limits](https://developer.tesla.com/docs/fleet-api/billing-and-limits)
states a $10 monthly developer discount, charges eligible requests with status
below 500, and describes a default account spending limit of zero. Auth endpoints
are documented as unbilled. Do not assume these facts are permanent or that a
particular account/vehicle discount applies.

**Local estimate:** 144 location attempts/day × 30 × $0.002 = **$8.64/month** if
always online at 600-second polling, or **$8.93 for 31 days**, excluding check-now
and retries. The $10 discount is stated for individual developers/small apps; it
is not a guaranteed $10 allowance dedicated to this controller. Never subtract
the discount in the device's estimate; the account portal is authoritative.
As of 2026-09-24, the controller caps `vehicle_data?endpoints=location_data` at
5,000 reserved attempts per UTC month, a $10 local pre-discount ceiling at the
published Data price. All status/location/refresh attempts also count against
configurable 400/day and 12,000/month defaults. Reserve four attempts per endpoint
in flash before use; unused reservations are lost on reset. Thus upper-bound
counts may exceed actual requests. Caps stop Tesla polling/lease renewal. An
authenticated check-now has a separate 10-minute cooldown. Other apps and price
changes can still consume or exceed the account discount.

## SDK and network contracts

[ESP-IDF stable entry](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/)
was consulted via the explicit [v5.5.2 guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/).
Firmware pins release **v5.5.2**, commit
`30aaf64524299d3bde422ca9a2848090d1bc5d0f`, including its Git submodules. Toolchain
versions come from that release's `tools/tools.json`, not a rolling Arduino core.
The release also needs the checked-in
`tools/patches/esp-idf-v5.5.2-https-oom.patch`: it deletes an unowned TLS session
when inbound transport-context allocation fails. This is a project-maintained
cleanup fix, not a different released SDK or a TLS verification exception.
Bootstrap applies it idempotently against the exact commit, and CMake rejects
an unpatched SDK.

- [ESP TLS](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/api-reference/protocols/esp_tls.html): nonblocking TLS with certificate bundle, hostname/SNI verification and time validation. No insecure certificate option, redirects, arbitrary URL input or proxy is provided.
- [NVS](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/api-reference/storage/nvs_flash.html): blob replacement + checked commit, application CRC/version and pending provisioning marker. No erase-on-error behavior. Actual brownout testing is still needed.
- [Task watchdog](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/api-reference/system/wdts.html): the control task subscribes and feeds only after processing deadlines and GPIO. Silent reset/core-dump omission avoids credential dumps.
- [HTTP server asynchronous requests](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/api-reference/protocols/esp_http_server.html): `httpd_req_async_handler_begin/complete` retain and release a request while a bounded worker verifies a password. `httpd_queue_work` returns session mutation to the server task. Contracts were checked against the pinned SDK headers/source and exercised on hardware.
- Local TLS uses an explicitly generated per-device certificate and local CA. Ordinary NVS is **not encrypted**; secure boot, flash encryption and irreversible eFuses are not enabled automatically.

Additional SDK verification on 2026-09-23:

- [ESP-NETIF SNTP lifecycle](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/api-reference/network/esp_netif_programming.html#sntp-service) recommends starting after network connection; starting earlier can trigger retry backoff. The firmware initializes without starting, then starts/restarts on `IP_EVENT_STA_GOT_IP`.
- The pinned SDK's `components/lwip/port/include/lwipopts.h` defines the reserved DNS index as `DNS_MAX_SERVERS - 1`. Its [lwIP DHCP implementation](https://github.com/espressif/esp-lwip/blob/fd432e4ee2cfb7f7f1c7eb7227e0173412e7b84e/src/core/ipv4/dhcp.c#L800) skips that slot even when fallback support is disabled. The project now allocates two slots for one usable DHCP resolver and an unused reserved slot, with a compile-time guard against a one-slot configuration. This is an SDK-specific implementation constraint, not a Tesla requirement.

Autonomy contracts rechecked on 2026-10-05 UTC: Tesla still documents three-month
refresh-token expiry and reuse of the most recently used token for up to 24 hours
on the linked third-party-token page. Its linked billing page states access is
restored when a new monthly cycle begins after a billing-limit suspension.
The project therefore retries up to 32 uncertain refresh attempts in the same
first-intent 24-hour window and releases only billing pauses at month rollover.
These are explicit recovery decisions, not permission to bypass consent/caps.
The six-hour SNTP holdover limit is also a project decision. The pinned SDK's
SNTP callback, delayed LOST_IP event, resolver and TLS source were inspected;
actual DHCP, RF and brownout failure behavior remains a hardware check.

## Optional retained-decision policy (project choice)

`outage_policy=hold_last` is an explicit owner-selected departure from the default
expiring authorization contract, not a Tesla interface feature. It retains the
last confirmed AUTO decision during missing data and outages and restores the
last durably saved HOME/AWAY decision after ordinary power loss. Startup is OFF;
saved HOME may resume after the minimum 30-second OFF dwell without internet/UTC.
Watchdog/panic reset, explicit OFF, permanent authorization loss and local faults
remain inhibitors. New evidence still needs its selected timestamp/identity
checks. This mode has no bounded departure cutoff during an outage and makes no
claim of fresh GPS or physical presence. Default/legacy records remain `expire`.
The NVS record and synthetic power-loss tests are project implementation evidence;
actual interrupted commits, startup pulses and brownouts require hardware checks.
See [the policy owner](architecture.md#holding-the-last-confirmed-auto-decision).

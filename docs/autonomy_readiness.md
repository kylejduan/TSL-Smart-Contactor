# Autonomous operation audit

This audit describes source behavior and synthetic recovery coverage. It is not
an installation diary or a claim that every hardware failure was measured.

## Verdict

The design automatically recovers from ordinary power, Wi-Fi, DNS and temporary
API outages when its prerequisites return. It cannot promise uninterrupted
charging or indefinite operation without maintenance. In particular, a reboot
or expired HOME lease requires a fresh qualifying location: a sleeping vehicle
cannot recreate permission. The controller deliberately never wakes the car.

Recovery fixes are included in source. Each installation must complete the
[bench checklist](bench_checklist.md); compilation and simulation cannot establish
unmeasured electrical or physical failure behavior.

## Recovery matrix

| Condition | Automatic response and recovery | Intervention boundary |
| --- | --- | --- |
| Power loss, normal reset or watchdog reset | Starts OFF; loads committed configuration/token/accounting; rejoins Wi-Fi; resynchronizes UTC; persisted AUTO may obtain new evidence after the 30-second OFF dwell. No lease or timed override survives. | DISABLED remains DISABLED. A sleeping car must naturally wake and supply fresh GPS, or the owner must act. Damaged flash/hardware is not recoverable by software. |
| Router, Wi-Fi or DHCP address loss | Retries Wi-Fi with a delay bounded at 60 seconds; DHCP loss also clears network/UTC readiness. Existing permission expires at its original deadline; a new IP requires a new SNTP sync. | Changed SSID/password or incompatible Wi-Fi security requires USB credential update. The local HTTPS URL/certificate must still match the reserved address. |
| DNS, Internet, TLS connect or transient API failure | Finite SDK resolver/socket work and bounded HTTP progress checks; exponential backoff to about an hour with jitter. Retry activity never extends permission. | Recovery can take up to the current backoff. Invalid trust roots/certificates require repair or a firmware update, never disabled verification. |
| NTP-only outage | New evidence and outbound TLS are inhibited once the last successful sync is more than six hours old. Existing monotonic lease/override deadlines do not move. SNTP continues retrying; successful synchronization restores eligibility. | Restore time-server reachability if both configured servers remain unavailable. NTP is not authenticated; this is a drift bound, not a defense against a hostile time source. |
| HTTP 429 or 5xx | Honors Retry-After and backoff, then retries through the same single worker and spending caps. | A refresh whose request may have reached Tesla also has the bounded token-recovery rule below. |
| Interrupted refresh/token response lost | Intent is durable before transmission. Up to 32 ambiguous attempts within 24 hours of the original uncertainty; replacement must commit before use. Proven-unsent failures restore only their own intent. | Exhausting the bound/window requires new consent. The device cannot reconstruct an unknown token indefinitely. A permanent `login_required`/revocation is not retried automatically. |
| Daily/monthly local request cap | Stops renewal; existing permission expires. Retries local accounting hourly and resumes after the appropriate UTC period changes. Records survive reboot. | Other apps/account usage and future pricing remain outside this device's estimate. No automatic spending-limit increase. |
| Tesla billing-limit response | Pauses in that billing month. A newer synchronized UTC month releases only this pause, while retaining Retry-After and local caps. | Payment/account problems may require portal action. Permission, repeated authentication, redirect and storage pauses are not cleared by month changes. |
| Temporary TLS/login allocation failure | Rejects that request, frees allocated resources and allows later retries. It does not permanently fault the independent control loop. Failed allocation size remains diagnostic. | Essential task/startup failure, failed cleanup, storage/configuration failure or control fault still inhibits output. |
| Invalid/stale/negative GPS, OFFLINE or ONLINE without GPS | Does not renew; known newer AWAY invalidates immediately. Fresh qualifying evidence can recover after dwell. | No offset fitting, report-time fallback or automatic policy switch. Persistent upstream timestamp faults may need Tesla support. |
| Long sleep at home | Explicit ASLEEP extends only a continuously live HOME lease, capped at 24 hours from the last qualifying source time. | After that ceiling, or after any lease expiry, fresh GPS is required. This intentionally sacrifices availability rather than guessing presence. |
| Corruption, interrupted provisioning, failed OFF/configuration commit | Stays inhibited and reports the fault; credentials are never automatically erased. | USB recovery is deliberate. Automatically rebooting or clearing this fault could reload an older AUTO setting after an uncommitted OFF, so no such recovery was added. |

## Findings repaired

1. Three ambiguous refresh attempts could exhaust recovery during a short service
   outage. The journal now permits 32 with the **same original 24-hour ceiling**.
   Both are bounded project policy; 32 is not a Tesla-published allowance. HTTP
   errors remain conservatively ambiguous. Existing records upgrade in place;
   older firmware rejects a pending record with more than three attempts, so do
   not downgrade during recovery.
2. A billing error paused forever unless manually retried. Only this pause now
   resumes in a new synchronized billing month. Other permanent errors remain
   paused. This follows Tesla's documented billing-cycle restoration.
3. A standalone refresh retained Retry-After from an older request. Each refresh
   now starts with its own hint; a coordinated 401 retry still retains its
   initiating request's hint.
4. DHCP address loss was not handled without a Wi-Fi disconnect. LOST_IP now
   inhibits network/evidence readiness until address acquisition and SNTP recover.
5. The global allocation hook made an otherwise handled TLS/login failure a
   permanent controller fault. Callers now determine severity; indispensable
   control/setup/storage failures remain critical, with no automatic fault clear.
6. The pinned SDK leaked a successful inbound TLS session when the following
   transport-context allocation failed. A one-line cleanup patch is checked and
   applied by bootstrap and CI; target configuration rejects an unpatched SDK.
7. An SDK connect call finishing after the application connect budget could
   previously proceed to send HTTP. Late completion is now rejected before any
   HTTP write, preserving proven-unsent token recovery. The synchronous DNS/select
   part of that SDK call remains finite but can overrun the nominal connect
   budget; it runs outside the control task.
8. UTC readiness could remain true indefinitely during an NTP-only outage.
   A six-hour monotonic sync-age limit now gates new evidence and TLS. USB
   diagnostics expose `utc_sync_age_s`; this is a project default, not an API rule.
9. A null observation queue was detected at startup, but the loop still attempted
   a receive using that handle. It now skips queue reads after creation failure
   while retaining the critical fault and OFF command. A native test injects
   this failure into the actual production entrypoint rather than a copied loop.

## Verification and limits

The production client/parser, journal/budget, scheduler and policy run together in
`tests/autonomy_tests.cpp` for 366 synthetic days. It includes recurring three-hour
Wi-Fi losses, four-hour API outages, daily HOME/AWAY and sleeping intervals,
automatic refresh rotation, 13 UTC month identities and uptime beyond the 32-bit
millisecond boundary. Assertions require automatic recovery, no authorization
after outage expiry, no sleeping-location requests and no cap overflow. There
are no paid requests or real credentials in this test. Separate tests cover
power loss around token commits, reboot/dwell, clock jumps, sync-age expiry,
revocation, retry bounds and billing/calendar behavior.

Linux/WSL tests also compile the production entrypoint and board adapter with
test-only SDK calls. Twenty-one process-isolated scenarios verify startup
inhibition, task/storage/queue failures, GPIO errors, OFF during blocked I/O,
lease expiry and control deadline/watchdog-feed failures. They verify command
and state publication before feeding the watchdog. They do not simulate
FreeRTOS concurrency or prove the physical watchdog's reset timing.

This simulation does **not** test flash wear, electrical contacts, real radio/DHCP
events, real TLS allocation failure, physical brownouts, RF interference or the
duration of ESP task scheduling stalls. Hardware fault injection and sustained
commissioned dry-run remain open in the bench checklist. See the appended
[verification record](verification.md) for exact command results and binary size.

Long-term maintenance remains necessary: the local leaf certificate is issued for
825 days and its local CA for 3,650 days; renewal is a deliberate USB setup action.
Tesla's refresh tokens expire after three months without successful replacement;
long DISABLED periods or extended outages can therefore require consent again.
The TLS trust bundle and SDK/security fixes are firmware-pinned and require USB
updates. There is no OTA updater or remote monitoring service, by design.

Primary contracts rechecked for this audit:
[Tesla token reuse and expiry](https://developer.tesla.com/docs/fleet-api/authentication/third-party-tokens),
[Tesla billing-cycle restoration](https://developer.tesla.com/docs/fleet-api/billing-and-limits),
and [ESP-IDF NVS](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/api-reference/storage/nvs_flash.html).
The exact pinned SDK sources were also inspected for SNTP, DNS, HTTP and TLS
cleanup behavior. Manufacturer/API facts and project decisions are distinguished
in [verified interfaces](verified_interfaces.md).


## Optional report profile

Native integration tests also cover an explicitly selected 600-second report-age
and lease profile with 540-second polling. They exercise a 36-hour sleeping
interval without fabricated location renewals, the fixed 24-hour source ceiling,
refresh rotation and resumption only after new qualifying ONLINE/HOME evidence.
A 31-day always-online simulation uses 4,960 location reservations; added usage
can exhaust the unchanged 5,000-Data cap. These are synthetic software checks,
not actual billing, sleep duration, contact or flash-wear measurements.

# Acceptance requirements

This record maps project requirements to implementation and verification. It does
not certify any particular installation. New devices remain uncommissioned,
DISABLED and dry-run until deliberately configured and bench-checked.

| Requirement | Implementation and offline evidence | Installation verification |
| --- | --- | --- |
| One-channel board and explicit output | `main/board.hpp`: GPIO47 active-high; inactive latch before output; one control owner | Confirm board/revision, relay polarity and startup behavior |
| Authoritative interfaces | `verified_interfaces.md`: primary sources, SDK pin, scopes/regions/schema and assumptions | Verify compatibility with the selected vehicle; invalid source time cannot authorize strict mode |
| Independent control deadlines | 50 ms control loop, separate network tasks, generation-fenced observations, progress watchdog; Linux tests execute the actual production entrypoint with fake SDK calls | Measure deadline latency under load and supervised watchdog recovery |
| Startup, arming and modes | Native commissioning/dry-run/boot/dwell/DISABLED/override/expiry cases | Observe contacts, reset during override and expiry without gratuitous pulses |
| OAuth and provisioning | Local registration/consent/USB helper, device-only refresh owner, checked NVS journal and bounded recovery | Verify live handoff/rotation and supervised power loss around commits |
| API scheduling and cost | Official TLS hosts, selected VIN, status before location, no wake/commands, single-flight/backoff/caps | Verify TLS rejection, connectivity recovery and Tesla account spending limit |
| Presence policy | Production parser/policy tests cover identity, freshness, order, hysteresis, leases, sleeping ceilings and clock changes | Compare real HOME/AWAY/sleep transitions with physical observations |
| Local management | USB-first recovery, per-device HTTPS, password/session/Origin/CSRF, redacted diagnostics | Install local CA and verify the actual LAN certificate and recovery path |
| Reliability | Sanitizers, synthetic transport/storage failures and endurance scenarios; production-entrypoint tests cover startup/queue/GPIO/deadline/watchdog-feed failures | Supervised brownout/watchdog/radio/contactor tests |
| Deliverables | Firmware, pinned build/partitions, helper, UI, fixtures, setup and bench guides | Complete and retain a private installation checklist |

`gps_source` remains the default acquisition-time policy. Explicit
`vehicle_report` selection deliberately accepts latest reported coordinates
without proving their acquisition age. It is not a GPS timestamp correction and
cannot guarantee shutoff within ten minutes of physical departure. No automatic
fallback or vehicle wake is provided.

Keep live logs, trip/charging history and hardware measurements in ignored private
storage. Publish only synthetic examples and sanitized, reproducible software
findings. Firmware is not electrical installation approval or contact feedback.

## Optional hold-last policy

`outage_policy=hold_last` trades the default bounded outage cutoff for continuity:
confirmed HOME survives missing observations, outages, sleep and cap exhaustion
until valid AWAY or explicit/local inhibition. Production native tests and browser
fixtures cover this alternative, including saved HOME/AWAY restoration after
startup dwell, commit inhibition/corruption, local-fault reset rejection and
unchanged request caps. Real outage and power-cycle measurements remain local
installation acceptance; no software result establishes physical presence.


Local signed OTA software is available in 0.2.0. This does not certify a device
installation: the [OTA checklist](ota.md#verification) requires an approved
isolated migration and supervised wireless/interruption/rollback checks. Firmware
self-tests establish local startup health, not electrical output or every possible
application regression. Shared NVS/token state is never rolled back.

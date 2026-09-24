# Requirement acceptance record

As of 2026-09-23. This is a software and integration audit, not electrical approval.
The original requirements remain the acceptance criteria. A native test or compiled
image does not satisfy a physical measurement or prove live GPS compatibility.

| Requirement group | Implementation / evidence | Remaining acceptance work |
| --- | --- | --- |
| 1. Fixed hardware and explicit output | `main/board.hpp`; GPIO47 active-high, inactive latch before output driver; no toggle API, no other relay pins | Inspect assembly and measure COM–NO/polarity/startup, contactor and electrical installation separately |
| 2. Authoritative interfaces | `verified_interfaces.md`; vendor schematic/example, official Tesla endpoints/scopes/regions, pinned SDK | No authoritative interpretation of the selected vehicle's negative GPS source time; no inferred conversion permitted |
| 3. Independent control | Production `policy.cpp`, 50 ms control task, one GPIO owner, separate network/USB tasks, progress watchdog | Web OFF during a competing login passed; supervised watchdog and worst-case timing measurements remain |
| 4. Modes/startup | Native boot, commissioning, dry-run, dwell, persistent DISABLED, volatile timed override, no-ON-pulse transition tests | Physical boot/reset/ROM-download pulse measurements and supervised relay transitions |
| 5. OAuth/provisioning | Local Python registration/consent/selection/handoff; device-only runtime rotation, journal, bounded recovery; actual consent/rotation/reboot passed | Actual power-loss testing across NVS rotation; reauthorization recovery remains owner-operated |
| 6. API/scheduling/costs | Exact VIN, official TLS hosts, status before location, no wake/commands, single worker, backoff/caps/reservations; exact boot attempt counters | Negative outbound TLS certificate/name/time tests on hardware; account charges/discounts remain Tesla-authoritative |
| 7. Presence policy | Native production policy/parser tests cover leases, source age, boundaries, duplicate/order/generation, sleep ceiling, clock jumps, override separation | Live fresh HOME/AWAY/sleep acceptance blocked by invalid GPS source time; historical GPS does not prove physical presence |
| 8. Local management/persistence | USB-first setup, per-device HTTPS, password/session/Origin/CSRF, bounded input/log, versioned NVS, no AP/OTA/unused services | No NVS encryption or physical-extraction protection claimed; no autonomous arming |
| 9. Reliability/tests | Native ASan/UBSan and Python suites, parser mutations, simulated storage/transport, actual USB/TLS/login diagnostics; no mock firmware transport | Physical brownout/watchdog/relay tests; native fake storage is not an NVS brownout measurement |
| 10. Deliverables | Source, pinned build, partitions, configuration example, helper, static callback/key-hosting site, README, architecture, fixtures, bench checklist and verification record | Full installation acceptance remains incomplete until above live and physical checks pass |

## Current live boundary

The ESP32 received location HTTP 200 after an ONLINE status. Identity/coordinate
validation passed. Original `gps_as_of` numeric text was negative, matching the
parsed value exactly. That evidence cannot establish a source-anchored HOME lease.
No other timestamp, unit guess, wrapping offset or receipt time is substituted.
Further blind repeated polling does not resolve this compatibility boundary.
An owner-authorized retry after driving still returned a negative source time.
The firmware now retains bounded request ID and timestamp metadata for a support
case; AUTO remains blocked pending valid GPS or an authoritative Tesla correction.

Keep DISABLED, uncommissioned and dry-run until the corresponding physical and
live checks actually pass. See [verification](verification.md) for measured facts
and [bench checklist](bench_checklist.md) for the remaining supervised steps.

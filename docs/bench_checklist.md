# USB-only bench checklist

Disconnect all contactor/mains wiring from the controller and supply USB power
only. Never measure continuity on an energized circuit. Obtain explicit operator
approval before flashing, relay ON tests or live Tesla requests. Electrical
installation approval is separate from firmware verification.

Copy this checklist into ignored private storage for each installation. Record
board revision, firmware/binary, instruments, results and measurements there.
Public documentation must not record personal trips, charging sessions or network
identifiers. Leave unperformed steps unchecked; software status is not feedback.

## Hardware, startup and commissioning

- [ ] Confirm SKU 32152, internal antenna, one channel and 5 V supply.
- [ ] Verify flash capacity is at least the configured 8 MB and 8 MB octal PSRAM is detected. Check the physical board revision.
- [ ] Preserve NVS and install production explicitly; remove factory demo controls.
- [ ] Observe COM–NO open before/after provisioning, with DISABLED, uncommissioned, dry-run and OFF commanded.
- [ ] Measure actual active-high GPIO47 polarity using the separately authorized [relay diagnostic](relay_bench.md); confirm closure during its pulse and reopening afterward.
- [ ] Check cold power-up, power reapplication, reset, ROM/download and firmware upload. Use suitable isolated measurement to detect pulses below meter resolution. Any unexplained closure blocks arming.
- [ ] Restore and hash-verify production after any diagnostic. Confirm inhibited output, intact configuration/token and automatic Wi-Fi/UTC recovery.
- [ ] Explicitly acknowledge `USB_BENCH_POLARITY_AND_STARTUP_VERIFIED` with `usb … arm`; commissioning must leave DISABLED and dry-run.

Firmware cannot guarantee GPIO behavior before its initialization. Record
inaccessible reset controls and measurement-resolution limits explicitly.

## Authentication, policy and recovery

- [ ] Run the production native sanitizer, Python and browser suites.
- [ ] Verify dry-run desired HOME/ON differs from physical OFF commanded.
- [ ] Confirm selected VIN/home and fresh source evidence for the explicitly selected policy. Strict mode requires GPS source seconds; report mode validates report milliseconds without proving GPS age.
- [ ] Verify read-only consent and no wake/charging/vehicle-command requests.
- [ ] Observe device-owned token replacement, checked commit and recovery after reboot. Do not run a competing refresh owner.
- [ ] Verify local certificate/hostname and negative outbound certificate, hostname and time checks.
- [ ] Verify unauthenticated, wrong-Origin and missing/wrong-CSRF state changes fail; rejected login attempts throttle.
- [ ] Issue OFF while an older authenticated USB AUTO request is pending; the older generation must be rejected.
- [ ] Exercise Wi-Fi recovery only in its inhibited state; verify tokens/TLS keys are preserved and failed storage inhibits output.

## Physical output, still isolated from mains

- [ ] Deliberately acknowledge `ALLOW_PHYSICAL_RELAY_AFTER_BENCH_CHECK` using `usb … enable_output`; verify it returns to DISABLED/OFF.
- [ ] After 30 seconds continuously OFF, select AUTO and request a short authenticated timed override. Observe isolated contact closure and reopening on immediate OFF.
- [ ] Confirm DISABLED rejects TIMED_ON and queued/late responses. Measure remaining OFF dwell before another ON; no minimum-ON hold may delay OFF.
- [ ] Reset during TIMED_ON: OFF initially, no restored override/lease, new qualifying evidence required for AUTO.
- [ ] Observe AUTO HOME closure and fresh AWAY reopening; compare explicit OFF with measured contacts.
- [ ] Let override expire without AUTO evidence: OFF. Separately maintain real HOME evidence in the background: expiry must retain ON without a gratuitous pulse.
- [ ] Interrupt Wi-Fi/DNS during HOME: permission expires at its existing deadline. On reconnect, new evidence/TLS remain inhibited until fresh SNTP readiness. Separately verify TIMED_ON keeps its own deadline during network loss.
- [ ] Verify stale/duplicate/ONLINE/OFFLINE/errors do not renew. Observe real ASLEEP extension of a live HOME lease; use native fake time for the full fixed source+24-hour ceiling and inability to resurrect expiry.
- [ ] In dry-run, temporarily lower caps and verify polling/renewal stop. Reboot must preserve period accounting; restore reviewed settings afterward.
- [ ] Verify supervised NVS power-loss behavior before/during/after token commits, preserving the only usable token and its bounded recovery window.
- [ ] With output inhibited, use supervised instrumentation to stall only the control task. Observe progress-watchdog reset/OFF timing; a debugger CPU halt is not equivalent evidence.
- [ ] Measure deadline latency under Wi-Fi/TLS/login/NVS load, heap/stack headroom and normal scheduling checks at least every 100 ms.

Return to DISABLED after testing. Complete separate electrical approval and
resolve unexpected behavior before connecting mains and deliberately selecting
AUTO. Firmware cannot detect welded contacts or identify the connected load.

## Optional hold-last acceptance

In an isolated USB-only setup, select `hold_last` deliberately. Confirm that a
qualifying HOME observation establishes AUTO, loss of Wi-Fi/API data beyond the
lease displays retained HOME, and a newer valid AWAY observation or explicit OFF
commands OFF. Do not infer these results from a software build alone. Check that
a power cycle begins OFF: saved HOME resumes only after the minimum 30-second
OFF dwell, including with internet unavailable; saved AWAY/DISABLED stays OFF.
Verify dry-run still keeps contacts open, TIMED_ON is not restored, watchdog/panic
recovery discards HOME, and pending/failed/corrupt commits are visible and inhibited.
Power cuts before/after an OFF/AWAY commit may preserve the old/new decision;
measure this boundary rather than assuming GPIO and flash change atomically.
Hold-last permits indefinite ON without departure confirmation. It does not
verify voltage, contactor movement, electrical protection or physical presence.

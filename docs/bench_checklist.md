# USB-only bench checklist

**Do this with all contactor/mains wiring disconnected from the controller.**
Supply only USB power for initial checks. Use a multimeter on the isolated relay
COM/NO contacts; never use continuity mode on an energized circuit. Do not infer
outlet voltage, charging or welded-contact detection from any software indicator.
Electrical installation/protection approval is a separate work item.

This checklist is **not complete**. Initial chip detection, flashing and USB status
checks are recorded in [verification evidence](verification.md#usb-board-check-2026-09-23).
Electrical/contact checks remain unperformed. Record board revision,
flash ID, firmware commit, dates, measurements and the exact tests you actually ran.
Do not mark a step passed from compilation or a simulated GPIO result.

## Before arming this firmware

- [ ] Confirm SKU 32152 / one channel / internal antenna and the 5 V board.
- [ ] Confirm the installed flash capacity with `esptool flash_id` and inspect the
  physical revision. The build uses 8 MB conservatively; the schematic labels a
  16 MB part. Verify the documented 8 MB octal PSRAM is detected; TLS allocations use it.
- [ ] Verify the actual board's active-high GPIO47 relay polarity using a supervised
  **vendor-approved USB-only bench procedure**, with no mains/contactor connection.
  The vendor demo can toggle at startup and must not be used as production firmware.
  Do not acknowledge polarity merely from the schematic. If this measurement is
  unavailable, leave this firmware uncommissioned/dry-run and obtain qualified
  bench assistance; there is intentionally no unarmed raw-GPIO ON endpoint.
- [ ] Flash this firmware explicitly; no factory relay services should remain.
- [ ] Before provisioning and after provisioning, COM–NO is open at steady state.
  UI/USB report uncommissioned, DISABLED, dry-run and OFF commanded.
- [ ] Check cold power-up, power removal/reapplication, RESET, ROM boot/download mode
  and firmware upload/reset. Measure COM–NO and, with suitable equipment, GPIO47 /
  relay coil. A brief pulse may evade a continuity meter; use a scope/logic analyzer
  or appropriate isolated measurement where needed. Firmware cannot guarantee the
  interval before its GPIO initialization. **Any unexplained pulse blocks arming.**

Only after the above actual observations, run `usb … arm` and type
`USB_BENCH_POLARITY_AND_STARTUP_VERIFIED`. This records commissioning while retaining
DISABLED and dry-run. The software cannot independently verify your measurement.

## Dry-run policy and authentication

- [ ] Run the native suite with sanitizers and the Python setup suite. Its synthetic
  HOME/AWAY, sleeping, expired, malformed, clock-jump and late-response transitions
  use the production policy/client/parser, with fake GPIO decisions/storage/time.
  They cannot enter the physical firmware transport or energize the real relay.
- [ ] Select AUTO while still dry-run. Check that desired HOME/ON and physical
  **OFF commanded** remain distinct. Live Tesla verification requires your own
  explicit credentials/consent and may incur calls. Bounded live checks reached
  the location endpoint but rejected its GPS timestamp; see the verification record.
- [ ] On a live, already-online vehicle near the configured home, verify the parser
  receives fresh `drive_state.gps_as_of` with correct seconds and source age.
  Do not substitute the general timestamp when GPS time is absent. Confirm actual
  VIN/home and distance. Wake the vehicle yourself if needed; firmware must not.
- [x] Verify read-only scope consent and the absence of wake/charging/command calls.
- [x] Observe an automatic token refresh and subsequent successful status poll.
  Verify its replacement survives a reboot without exposing either token.
  Native tests cover power loss before/after commit; actual brownout/NVS testing
  remains separate and may consume a token's recovery window. Never reuse that
  token concurrently from the laptop.
  Completed during the 2026-09-23 bounded live checks and subsequent reboots;
  this does not establish valid GPS-source time or physical brownout behavior.
- [ ] Confirm a TLS-untrusted certificate, wrong hostname and invalid time fail
  outbound requests. Confirm the device's local certificate matches its LAN URL.
- [ ] Signed-out state changes, missing/wrong CSRF and wrong Origin must fail.
  Login failures throttle; status requests alone must not trigger paid Tesla calls.

## Physical output checks, still disconnected from mains

- [ ] Explicitly run `usb … enable_output`; type
  `ALLOW_PHYSICAL_RELAY_AFTER_BENCH_CHECK`. It returns to DISABLED. Recheck OFF.
- [ ] Select AUTO; after 30 seconds OFF, request a short authenticated override:
  `usb … timed_on --seconds 60`. Confirm relay energizes and isolated COM–NO closes.
  This is a **manual override test**, not synthetic vehicle evidence. User confirmation
  is required by the helper. The previously measured polarity must agree.
- [ ] Issue OFF immediately. COM–NO opens promptly. Attempting TIMED_ON while
  DISABLED must fail. After selecting AUTO again, ON must wait for the remaining
  30-second OFF dwell. There is no minimum-ON hold.
- [ ] Reset during TIMED_ON. Relay returns OFF; the override and old AUTO lease are
  gone. An armed AUTO boot still needs new qualifying GPS.
- [ ] Let the timed override expire without AUTO evidence: OFF. In a separate test,
  maintain fresh real HOME evidence in the background: expiry retains ON without
  a gratuitous OFF pulse.
- [ ] For actual AUTO HOME, interrupt Wi-Fi/DNS. Existing permission expires at its
  existing source-anchored deadline; reconnect/retries alone do not extend it.
  TIMED_ON deliberately remains effective during connectivity loss until its own
  deadline. Test these as separate cases.
- [ ] Verify a fresh AWAY result turns AUTO OFF and stale/duplicate/ONLINE/OFFLINE
  results do not renew. For long sleep behavior, confirm the fixed source+24 h
  ceiling; use the native fake-time test to cover the full interval efficiently.
- [ ] Set low request caps temporarily in dry-run, reach the cap, and verify polling
  stops and the existing lease expires. Reboot must not reset period accounting.
- [ ] Verify control-path watchdog recovery with supervised instrumentation that
  stalls **only the control task** while other tasks remain healthy. Keep the relay
  inhibited for initial fault injection. ESP-IDF may disable watchdog behavior when
  a debugger halts the CPU, so a generic debugger breakpoint is not proof. No public
  web/USB crash-injection endpoint is provided. Record observed reset/OFF timing,
  not only a watchdog configuration value.
- [ ] Measure control latency under Wi-Fi/TLS/login/NVS load and confirm ≤100 ms
  normal-scheduling deadline checks. Record stack high-water/heap headroom on the
  actual board. Compilation does not validate runtime memory pressure.

Before connecting the finished mains installation, return to DISABLED, resolve all
unexpected behavior, complete the separate electrical approval, and deliberately
select AUTO only when ready. Keep an appropriate physical disconnect available.

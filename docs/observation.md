# Local observation recording

The firmware exposes enough diagnostics to review authorization, software ON/OFF
decisions, ordinary Fleet polling and controller health. It does not measure
outlet voltage, load current, energy delivered or actual charging. Those require
physical observation; do not label an ON command as verified charging.

The dashboard and authenticated `/api/status` show:

- AUTO HOME, desired output, ON/OFF command, decision reason, lease and override.
- Vehicle ONLINE/ASLEEP/OFFLINE, selected timestamp basis, observation age,
  reported distance, last successful poll and next poll.
- Wi-Fi/UTC readiness, errors and reauthorization indication, uptime, generation,
  maximum control scheduling gap, free internal heap and build versions.
- Per-boot attempts, conservative persistent daily/monthly reservations and the
  local monthly Data cap. Reservations are estimates, not Tesla account charges.

Authenticated `/api/events` contains the latest 16 decision changes with uptime,
reason and ON/OFF command. It is RAM-only and resets on reboot. If more than 16
changes occur between reads, older changes are unavailable. Last-response
diagnostics can be overwritten by a later refresh; they are not a full API log.

## Optional computer recorder

`tools/observe_controller.py` writes private JSON Lines snapshots for a bounded
window. It only calls local HTTPS login-info, login, status, events and session
logout. It has no Fleet client, check-now, wake, mode, override or configuration
action. Reading status does not alter the device's Tesla polling cadence or budget.
It is a diagnostic aid, never an ongoing control dependency or telemetry receiver.

From the repository root on Linux/WSL, replace `<DEVICE_IP>` with the device's
reserved private IPv4 address and use the CA from your provisioning directory:

```sh
.venv/bin/python tools/observe_controller.py \
  --origin "https://<DEVICE_IP>" \
  --ca provisioning/local-ca.pem \
  --output-dir .tools/observations \
  --duration-hours 16 --interval-s 60
```

The password prompt does not echo. For unattended capture, supply
`--password-file <protected-local-file>` with exactly one `LOCAL=...` entry.
Never put the password itself in command-line arguments. Other entries, including
Tesla credentials, are ignored. POSIX core dumps are disabled before reading the
password. Python cannot guarantee RAM erasure; protect the computer/password file
and do not collect process memory dumps. The helper uses Python's standard library and
existing project helpers; no additional package or firmware update is needed.
On Windows, the same command can use `py -3` instead of `.venv/bin/python`; the
recorder restricts its output directory's Windows ACL to the current user.

The printed observation filename is unique. Existing files are never overwritten.
Default sampling is once a minute for 16 hours, with duration limited to 48 hours,
interval limited to 30–600 seconds and each file limited to 32 MiB. Files are
flushed and synced after each record, with Linux directory/file permissions
0700/0600. Keep recordings under ignored `.tools/`; they contain operational
history even though VIN, coordinates, credentials and session tokens are excluded.

The recorder validates the device CA, hostname and validity dates. It rejects
redirects, external origins, compressed or oversized replies and malformed JSON.
It reauthenticates after session expiry or a controller restart, once per sample;
transient connection failures are recorded and retried with a bounded delay up to
five minutes. Rejected credentials stop the recorder instead of causing a retry
storm. Reduced uptime and generation changes are recorded for later investigation;
they do not identify a reset cause. An observer connection error is not proof that
the controller itself failed or commanded OFF. Gaps can also result from computer
sleep or network loss, and a reboot may erase events before they are sampled.

The controller currently supports one administrator session. Recorder login and
reauthentication can sign out an open browser dashboard; a browser login can in
turn replace the recorder session. Avoid using both simultaneously during capture.
This affects management sessions, not AUTO or local OFF eligibility.

Keep the recording computer awake and on the home LAN. Do not change router ports,
sleep settings or network services merely to enable recording. A foreground capture
stops with Ctrl+C and attempts authenticated session logout. For a deliberately
backgrounded capture, keep its PID and stop that exact process with `kill -TERM
<PID>`; the bounded deadline also ends it automatically. The controller continues
its existing operation when the recorder stops. No automatic alerts or remote
Internet dashboard are provided.

For an overnight/departure test, let the ordinary scheduler operate. Record the
physical charging indication, approximate departure/arrival times when safely
parked, and any known power/network interruptions. Later compare the saved HOME,
ASLEEP/AWAY, lease expiry and command events against those observations. The logs
cannot establish unobserved electrical state or a general physical-departure bound.

Offline verification:

```sh
.venv/bin/python -m unittest discover -s tests -p 'test_observe_controller.py' -v
```

Synthetic tests cover route restrictions, redaction, reply limits, credential
failure, session recovery, offline/reboot recording and bounded file storage.

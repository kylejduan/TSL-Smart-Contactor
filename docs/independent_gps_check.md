# Independent Fleet GPS check

`tools/independent_gps_check.py` is a one-shot laptop HTTPS client and JSON parser
for comparing Fleet source-time diagnostics. It does not modify firmware, Wi-Fi,
commissioning or output. Run only with explicit permission while the vehicle is
already ONLINE: it permits at most one status and one location request, never
wakes the car and may incur Fleet charges.

It requests fresh read-only consent without `offline_access`, never refreshes
its access token and discards any unexpected refresh token. The ESP32 remains
the sole runtime refresh owner. Enter the application secret, selected VIN and
callback URL only in hidden local prompts. Optionally provide a protected file
with exactly one `TESLA_CLIENT_SECRET=...` entry via `--client-secret-file`;
never put the secret itself in a shell argument.

```sh
.venv/bin/python tools/independent_gps_check.py --directory provisioning
```

The helper validates state/redirect, exchanges the code, checks status and
requests location only if ONLINE, without automatic retries. It saves a private
0600 report with original numeric text, parsed number, report timestamp, HTTP
metadata and request timing. It omits full vehicle responses, VIN, coordinates
and tokens. Provisioning/private records must stay ignored. Python cannot
promise RAM erasure; close the helper and protect the local computer.

A negative literal reproduced independently rules out the ESP32 parser for that
transaction; it does not locate a vehicle/backend defect or establish an epoch
correction. A plausible value from one client needs a closely timed comparison
before attributing differences to another. No diagnostic authorizes a timestamp
conversion or an automatic policy change. Use the
[generic support template](tesla_gps_support_draft.md) with private attachments.

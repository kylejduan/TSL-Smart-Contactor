All fixtures and tokens in tests are synthetic. No real address, VIN, account,
GPS sample, credentials or paid Tesla request was used. The zero-coordinate home
is a mathematical test origin, never a deployment default: an unconfigured VIN
fails validation, and the helper explicitly asks for both home coordinates.

`home.synthetic.json` deliberately has a newer general millisecond timestamp than
its GPS source time. Production accepts only `drive_state.gps_as_of` as the source
seconds. Tests build additional malformed, stale, asleep, offline, rotated-token,
and HTTP-framing responses in memory. Native tests link the production policy,
JSON parser, HTTP decoder, Fleet client, token journal, budget and session code.

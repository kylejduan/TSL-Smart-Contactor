All fixture identities, coordinates, credentials and timestamps are synthetic.
Automated tests make no paid Tesla requests and contain no real capture metadata.
The zero-coordinate home is a mathematical origin, never a deployment default.
Unconfigured VIN/home fails validation and the helper asks for both coordinates.

Fixtures distinguish GPS source seconds from newer general report milliseconds.
Strict mode accepts only valid `drive_state.gps_as_of`; explicitly selected
report mode accepts `drive_state.timestamp` without proving GPS acquisition age.
Additional malformed, stale, sleeping, offline, rotated-token and HTTP-framing
responses are constructed in memory. Native tests link production logic.

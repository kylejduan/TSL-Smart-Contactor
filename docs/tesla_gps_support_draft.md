# Fleet source-time support template

Use this template privately through the
[Tesla developer dashboard](https://developer.tesla.com/dashboard). Fill placeholders
locally; do not commit the completed report. Do not include OAuth tokens, client
secret, Wi-Fi credentials, local passwords or private keys. The program does not
send support messages automatically.

**Subject:** Fleet REST location source timestamp validity and semantics

**Description:** We use the official regional Fleet API with read-only
`vehicle_device_data` and `vehicle_location` scopes. We request
`GET /api/1/vehicles/{vin}` and only when ONLINE request
`GET /api/1/vehicles/{vin}/vehicle_data?endpoints=location_data`. No wake or
vehicle commands are sent. Strict authorization requires GPS evidence no older
than the configured maximum source age.

For transaction `<private x-txid>`, `<HTTP status>` was returned with original
`response.drive_state.gps_as_of` text `<raw source value>` and general
`drive_state.timestamp` `<report value>`. An independent client/parser
`<did/did not>` reproduce the original number. Actual vehicle firmware is
`<version>`; API schema version, if returned, is `<version>`.

Please clarify the REST field's units, epoch, signedness and validity/reset
conditions. Is the general report timestamp the coordinate acquisition time or
report generation time? Is there a supported read-only field or conversion that
bounds actual GPS age? We cannot fit an offset to receipt time or silently
substitute report time for acquisition time. Please identify affected firmware
and the smallest additional diagnostic capture needed.

Attach the application client ID, selected VIN, transaction metadata and any
requested redacted response only through the private support channel. Preserve
original response bytes locally if capturing them; do not reconstruct a raw
response by serializing a parsed object. Keep trip/location history private.

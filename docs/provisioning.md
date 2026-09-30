# Provisioning the logger's hub-link credentials

The hub's AP (SSID + passphrase), the MQTT username/password, the API token
(`X-Hub-Token`) and the hub CA certificate live in **NVS** (namespace
`cardiaglink`), not in the firmware. They are set once over serial and survive
every app upload; only a full flash erase clears them. Nothing is compiled in:
with no SSID/passphrase stored the logger boots **STANDALONE** (own AP, no hub
join) and says so; with no token, `POST /api/v1/time` and `/files/ack` answer
401.

Values are never echoed. Everything the board prints is a fingerprint:
`sha256:` + the first 8 hex of SHA-256 of the value. Check one locally with

    printf %s 'value' | sha256sum | cut -c1-8
    openssl x509 -in ca.pem -noout -fingerprint -sha256     # CA: first 4 bytes

## Commands

A line starting with `:` is a command; it ends at Enter.

| Command | Effect |
|---|---|
| `:cred show` | fingerprint of each stored field, or `(not set)` |
| `:cred set ssid\|pass\|mqtt_user\|mqtt_pass\|token <value>` | validate, save |
| `:cred ca` | then the PEM; saved at `-----END CERTIFICATE-----` |
| `:cred clear <field>` / `:cred clear all` | remove |
| `:cred import` | one-time copy from a legacy `secrets.h` into an empty store |

Changes apply after a reset (esptool `--after hard_reset`, see CLAUDE.md).

## Provisioning with `serial_capture.py`

`--send` echoes what it sends into the log, so secrets go through
`--send-file`, which logs only a line count and a fingerprint.

1. Copy `tools/provision.example.txt` to `provision.txt` (gitignored, as is
   `*.pem`) and fill it in. Drop the lines you don't need.
2. Send it, then reset and read the banner:

       python tools/serial_capture.py --port COM3 --seconds 15 --send-file provision.txt
       python tools/serial_capture.py --port COM3 --seconds 15 --reset

3. The boot banner shows `[creds] hub link PROVISIONED (NVS):` and a fingerprint
   per field. Delete `provision.txt`.

## Migrating from `secrets.h`

A board built with the old `WIFI_STA_SSID` / `WIFI_STA_PASS` /
`HUB_API_TOKEN` in `secrets.h` can copy them once:

    python tools/serial_capture.py --port COM3 --seconds 10 --send ":cred import`n"

(PowerShell; `` `n `` is the Enter. `:cred show` works the same way -- it
prints fingerprints only, so `--send` is safe for it.)

The import runs only if the NVS store is empty and it has never run before,
and skips placeholder values. Afterwards remove those three defines (keep
`WIFI_AP_PASS`, the board's own AP password) and rebuild.

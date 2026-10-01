# Provisioning the logger's hub-link credentials

The hub's AP (SSID + passphrase), the MQTT username/password, the API token
(`X-Hub-Token`) and the hub CA certificate live in **NVS** (namespace
`cardiaglink`), not in the firmware. They are set over **serial only** -- no
HTTP or Wi-Fi route reads or writes them -- and read from NVS once, at boot.

- **No SSID/passphrase committed:** the logger boots **STANDALONE** (own AP, no
  hub join, no scanning) and prints `[creds] hub link NOT PROVISIONED` once.
- **No token:** `POST /api/v1/time` and `/files/ack` answer 401.

## What survives what

| Action | Credentials |
|---|---|
| `pio run -t upload` (bootloader, partition table, otadata, app) | **kept** -- NVS at 0x9000 is not written |
| `pio run -t uploadfs` (LittleFS partition) | kept |
| `pio run -t erase`, `esptool.py erase_flash` | **wiped** -- re-provision |
| a partition-table change that moves `nvs` | wiped (keep it at 0x9000/0x5000) |

## What the board prints instead of values

| Field | Shown as | Why |
|---|---|---|
| `pass`, `mqtt_pass`, `token` | `set, N chars` | A hash of a low-entropy passphrase lets anyone with a serial log test guesses offline. The length is enough to catch a truncated or space-padded paste. |
| `ssid`, `mqtt_user` | `sha256:` + 8 hex | Not secrets, just kept out of pasted logs. Check: `printf %s 'value' \| sha256sum \| cut -c1-8` |
| `ca` | `sha256:` + 8 hex of the DER | The standard CA fingerprint: first 4 bytes of `openssl x509 -in ca.pem -noout -fingerprint -sha256` |

The Wi-Fi driver's INFO log (which names the SSID on connect) is lowered to
WARN at boot. After provisioning, grep your first capture for the SSID and
the passphrase; neither should appear.

## Commands

A line starting with `:` is a command; it ends at Enter. Nothing is echoed.

| Command | Effect |
|---|---|
| `:cred show` | stored fields, commit state, and anything staged |
| `:cred set ssid\|pass\|mqtt_user\|mqtt_pass\|token <value>` | validate and **stage** |
| `:cred ca` | then paste the PEM; it is staged at `-----END CERTIFICATE-----` |
| `:cred clear <field>` / `:cred clear all` | stage a removal |
| `:cred commit` | check the whole set, then write it to NVS |
| `:cred abort` | drop everything staged |
| `:cred import` | one-time copy from a legacy `secrets.h` (see below) |

**Values are literal.** Everything after the single space that follows the
field name, up to the end of the line: spaces (including leading and trailing)
and quotes are part of the value, and there is no escaping. An editor that
leaves a trailing space changes the passphrase; `set, N chars` shows it.

**Limits.** SSID 1-32 bytes; passphrase 8-63 printable ASCII or 64 hex;
MQTT user 1-64 without spaces; MQTT password 1-128; token 8-128 without
spaces; a console line 160 bytes. The CA is **one** PEM certificate pasted as
lines (not one base64 line), 2048 bytes max -- the hub's EC P-256 CA is about
0.7 kB, and the whole NVS partition is 20 kB shared with the boot counter.

**Commit is all-or-nothing.** It refuses, writing nothing, if the resulting
set has an SSID without a passphrase (or MQTT user without password, or the
reverse), if NVS is nearly full, or while the change log is recording
(stop it with `l` first: flash writes stall the CPU). It writes
`complete=0`, then the fields, then `complete=1`. A power cut part-way leaves
`complete=0`, and the next boot says `INCOMPLETE` and runs standalone with the
token locked. Commit and a reset are the only times NVS is touched.

Changes apply after a reset (esptool `--after hard_reset`, see CLAUDE.md).

## Provision, check, clear (`serial_capture.py`)

`--send` echoes what it sends into the log, so secrets go through
`--send-file`, which logs only a line count and a fingerprint of the file.

1. Copy `tools/provision.example.txt` to `provision.txt` (gitignored, as is
   `*.pem`) and fill it in. Delete lines you don't need, but keep
   `:cred commit`.
2. Send it, check it, reset, read the banner:

       python tools/serial_capture.py --port COM3 --seconds 20 --send-file provision.txt
       python tools/serial_capture.py --port COM3 --seconds 15 --reset

   Expect `commit: complete` and `[creds] hub link PROVISIONED (NVS):`.
3. Delete `provision.txt`.

Check at any time (prints no values, so `--send` is fine; PowerShell,
`` `n `` is the Enter):

    python tools/serial_capture.py --port COM3 --seconds 5 --send ":cred show`n"

Clear everything, or one field:

    python tools/serial_capture.py --port COM3 --seconds 5 --send ":cred clear all`n" --send ":cred commit`n"
    python tools/serial_capture.py --port COM3 --seconds 5 --send ":cred clear token`n" --send ":cred commit`n"

## Migrating from `secrets.h`

A build whose `secrets.h` still defines `WIFI_STA_SSID` / `WIFI_STA_PASS` /
`HUB_API_TOKEN` can copy them once:

    python tools/serial_capture.py --port COM3 --seconds 10 --send ":cred import`n"

It runs only if the NVS store is empty and has never imported before, so it
never overwrites. It skips placeholder values, and a run that finds nothing
real does not use up the one time. It commits like any other change. Then
remove those three defines (keep `WIFI_AP_PASS`, the board's own AP password)
and rebuild.

## Coredumps contain credentials

The active SSID, passphrase and token sit in RAM, so a crash dump
(`GET /api/v1/coredump`, token-protected) can contain them. **Coredumps stay
private:** fetched by the hub only, never attached to a public issue or
committed. Share a decoded backtrace, not the dump.

## TLS / MQTT (not built yet)

The logger has **no MQTT or TLS client yet**. The MQTT credentials and CA are
stored for it and nothing reads them. When it is added, it must verify the
broker certificate against the provisioned CA **with a hostname check**. The
Pi's server cert (carhub `deploy/pi/bootstrap.sh`) carries
`DNS:carhub, DNS:carhub.local, DNS:localhost, IP:127.0.0.1, IP:10.42.0.1`:
10.42.0.1 covers the Pi's own AP in the car, but a laptop-hotspot address on
the bench is not in it. Because the bench address keeps changing, the client
should connect by IP and verify the name `carhub` (set the expected hostname
explicitly), rather than the SAN chasing every address.

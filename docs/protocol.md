# Logger ↔ Hub protocol v1 — logger-side mirror

**Source of truth: `carhub/docs/protocol.md`.** This file mirrors the sections the
logger implements so firmware work does not need the other repo open. If the two
disagree, carhub wins and this file is the bug.

## Poll plan — `POST /api/v1/pollplan`

The logger does not know the car (rule 3). What MODE_POLL asks for is the hub's
decision.

| | |
|---|---|
| Auth | `X-Hub-Token` (constant-time compare; 401 if wrong or missing) |
| Body | `{"version":1,"entries":[{"mode":1,"pid":12,"period_ms":200}, ...]}` |
| `version` | `1`. Anything else → 422 `unsupported version` |
| `mode` | v1 accepts **1** (OBD Mode 01) only → else 422 `unsupported mode`. Mode `0x22` is reserved for v2 with a `did` field |
| `pid` | 0–255 |
| `period_ms` | 100–60000 |
| entries | at most 32; a duplicate `(mode,pid)` → 422; **empty = clear the plan** |

Responses: `200 {"ok":true,"hash":"<16 hex>"}` · `400` malformed JSON ·
`401` bad token · `422 {"ok":false,"error":"<reason>"}`.

Logger 422 reasons: `unsupported version`, `unsupported mode`, `pid out of range`,
`period_ms out of range`, `too many entries`, `duplicate entry`, `missing field`,
`unknown field`, `duplicate field`, `invalid type`, `body must be an object`.

**Hash.** SHA-256 over the canonical form: entries sorted by `(mode,pid)`, each
serialized with keys in the order `mode,pid,period_ms`, as compact JSON (no spaces)
of the full `{"version":1,"entries":[...]}` object. Report the first 16 hex chars.

**Shared test vector** (identical in carhub's tests):

```
canonical  {"version":1,"entries":[{"mode":1,"pid":5,"period_ms":1000},{"mode":1,"pid":12,"period_ms":200},{"mode":1,"pid":13,"period_ms":500}]}
hash       3046906ac3000655
empty      {"version":1,"entries":[]}  ->  8bcae181d21321b7
```

`GET /api/v1/session` adds
`"pollplan":{"hash":"<16 hex>"|null,"entries":N,"source":"nvs"|"none"}`.
After a clear: `{"hash":null,"entries":0,"source":"none"}`.

The plan persists in NVS (namespace `pollplan`), which an app upload does not
touch: it survives reboot and a normal reflash. Only a full flash erase clears it.

**Logger behaviour.** MODE_POLL runs the plan with a non-blocking scheduler: one
request in flight, most-overdue entry next, `OBD_INTER_REQUEST_MS` between
requests, `OBD_RESPONSE_TIMEOUT_MS` per request, and per-PID backoff on no
response (`period << misses`, capped at `POLL_BACKOFF_MAX_MS` or the period if
longer). Every transmit goes through the TX gate (`cantx.h`), so nothing is sent
while the ignition is off. With no plan, MODE_POLL transmits nothing and says so.
Replies are not decoded on the logger; they are recorded as raw frames.

## Tier C trip bookend — record type 3, format `cdgb1`

Replaces "csv, not implemented". Raw: the logger does not decode (rule 3). The hub
turns DTCs into P/C/B/U text, takes MIL from bit 7 of PID 01 byte A, readiness from
bytes B–D, and scales Mode 06 values by UASID.

**File.** One closed file per event, kind `bookend`, tier C, format `cdgb1`:

```
file header, 16 B   "CDGB" | u8 version=1 | u8 rec_type=3 | u8 mode | u8 rsv=0
                    | u32 device_id | u32 boot_id
record              header 16 B + payload
```

**Record** (`rec_type` 3 — 1 and 2 are the UDP CAN/OBD records), little-endian:

```
header   u8 rec_type=3 | u8 version=1 | u16 payload_len
         | u32 device_id | u32 boot_id | u32 ms (relative, this boot)
payload  u8 kind (1 trip_start, 2 trip_end) | u8 flags | u16 reserved=0
         | u8[4] pid01_raw            Mode 01 PID 01 bytes A-D; all 0xFF if no response
         | u8 n_stored  | n_stored  x u16 DTC     Mode 03
         | u8 n_pending | n_pending x u16 DTC     Mode 07
         | u8 n_m06     | n_m06 x {u8 mid, u8 tid, u8 uasid, u16 value, u16 min, u16 max}
flags    bit0 complete | bit1 truncated (time budget, or a cap) | bit2 no-response
caps     32 stored, 32 pending, 64 Mode 06 results; overflow sets bit1
```

A DTC is the two bytes as received (`b0 << 8 | b1`), stored as a little-endian u16,
so the hub reads the u16 and takes the category from its top two bits. Mode 06 values
are big-endian on the wire and stored as little-endian u16. `pid01_raw` comes from the
first ECU that answers; DTCs and Mode 06 results are collected from every responder
(0x7E8–0x7EF) and kept raw (not de-duplicated).

**Shared test vector** (identical in carhub's decoder tests): device_id `0x11223344`,
boot_id 7, ms 123456, trip_start, flags complete, pid01 `81 07 65 00`, stored
`[0x0133]`, pending `[0x0420, 0xC123]`, Mode 06
`[(01,80,0A,0123,0000,0400), (21,87,24,8000,0010,FFFF)]` → 51 bytes:

```
03012300443322110700000040e20100010100008107650001330102200423c10201800a23010000000421872400801000ffff
```

**When the logger reads them.**

| Event | Trigger | Time budget |
|---|---|---|
| `trip_start` | ignition ON and the first answered poll-plan request | `BOOKEND_START_BUDGET_MS` (5 s) |
| `trip_end` | ignition input reads OFF, **before** the debounce confirms it (the TX gate follows the debounced level, so this is the only window that may still transmit) | what is left of `IGN_OFF_DEBOUNCE_MS` minus `BOOKEND_END_MARGIN_MS` |

Reads, in order: 01/01, 03, 07, then 06/00 (+ 06/20, 06/40, … while the range bit is
set) and every supported MID. Multi-frame replies use ISO-TP with a Flow Control to the
responder's physical id. A read that cannot start with `BOOKEND_MIN_START_MS` left is
not started; whatever did not fit sets the truncated flag. A `trip_end` with **no**
completed read is **skipped and logged**: an empty record would claim "no DTCs at
key-off", which nothing supports. An OFF that recovers (a cranking dip) discards the
`trip_end` in progress. Under the bus-quiet power policy there is no ignition input,
so only `trip_start` is written.

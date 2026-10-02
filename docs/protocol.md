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

carhub's plan vector (§2.6, `test_carhub_plan_vector`):

```
canonical  {"version":1,"entries":[{"mode":1,"pid":5,"period_ms":2000},{"mode":1,"pid":12,"period_ms":200},{"mode":1,"pid":13,"period_ms":500}]}
hash       e52fa6872718765d
```

`GET /api/v1/session` adds
`"pollplan":{"hash":"<16 hex>"|null,"entries":N,"source":"nvs"|"none"}`.
No plan (never set, or cleared): the **empty-plan hash**,
`{"hash":"8bcae181d21321b7","entries":0,"source":"none"}`, so POST and session
always agree. `hash` is `null` only when NVS is unreadable.

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
         | u8[4] pid01_raw            Mode 01 PID 01 bytes A-D from ECU 0 (0x7E8) ONLY;
                                      all 0xFF + bit2 if ECU 0 did not answer
         | u8 n_stored  | n_stored  x {u8 ecu, u16 dtc}        Mode 03   (3 B each)
         | u8 n_pending | n_pending x {u8 ecu, u16 dtc}        Mode 07   (3 B each)
         | u8 n_m06     | n_m06 x {u8 ecu, u8 mid, u8 tid, u8 uasid,
                                   u16 value, u16 min, u16 max}  Mode 06 (10 B each)
flags    bit0 complete | bit1 truncated (time budget, or a cap)
         | bit2 no-response (ECU 0 gave no Mode 01 PID 01 reply)
caps     32 stored, 32 pending, 64 Mode 06 results; overflow sets bit1
```

`ecu` = response CAN id − 0x7E8 (0 = engine). Entries are collected from every
responder (0x7E8–0x7EF) and **not** de-duplicated across ECUs. A DTC is the two bytes
as received (`b0 << 8 | b1`), stored as a little-endian u16, so the hub reads the u16
and takes the category from its top two bits. Mode 06 values are big-endian on the
wire and stored as little-endian u16. Largest record: 16 + 843 = 859 bytes.

**Shared test vector** (identical in carhub's decoder tests): device_id `0x11223344`,
boot_id 7, ms 123456, trip_start, flags complete, pid01 `81 07 65 00`, stored
`[(0, 0x0133)]`, pending `[(0, 0x0420), (1, 0xC123)]`, Mode 06
`[(0, 01,80,0A,0123,0000,0400), (1, 21,87,24,8000,0010,FFFF)]` → 56 bytes:

```
03012800443322110700000040e20100010100008107650001003301020020040123c1020001800a2301000000040121872400801000ffff
```

**carhub's vector** (carhub `docs/protocol.md` §1.7, c626e3a — the logger's encoder
matches it byte for byte, `test_carhub_bookend_vector`): device `0x744CF002`, boot 42,
ms 1500, trip_start, flags 0x01, pid01 `82 07 65 24`, stored
`[(0,0x0420), (0,0x0301)]`, pending `[(1,0xC123)]`, Mode 06
`[(0, 01,80,0A, 3000,0,5000), (0, 21,80,84, 0xFF9C,0xFE0C,0x01F4)]` → 56 bytes:

```
0301280002f04c742a000000dc050000010100008207652402002004000103010123c1020001800ab80b00008813002180849cff0cfef401
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

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

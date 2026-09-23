# Firmware plan — hub integration (protocol v1)

Implements `carhub/docs/protocol.md` v1 on the ESP32-CAN-X2. Written 2026-09-22,
before any code, against firmware at `15d88f3` (1,947 lines across 6 modules).

## 🔴 Scope reality: the SD half is blocked on hardware

`/api/v1/files*` assumes **indexed files**. The firmware today has **one volatile
in-RAM change log** — `recorder.h` says so outright: *"Both are VOLATILE — power loss
loses them. Persisting to flash or SD is a later tier."*

And the microSD slot is **on the carrier board, not the dev board**:

| Fact | Source |
|---|---|
| microSD is `J2`, `DM3AT-SF-PEJM5`, push-push | `Carrier Board — Requirements and BOM.md:563` |
| SD cannot share the MCP2515 SPI bus; needs its own | same note, line 250 |
| microSD is "Phase 2+" | `docs/hardware.md:34` |
| Carrier board ETA ~9/21 | **passed; arrival unconfirmed** |

⇒ **Split the work.** Phase A needs no SD and is testable at the desk today.
Phase B needs a file store.

### The unblock: back the file store with LittleFS now, SD later

`docs/hardware.md:34` claims 8 MB of internal flash holds ~28 hrs of Tier B.
⚠ **That figure does not survive measurement — see §Retention reality below; the real
buffer is ~6.6 minutes.** LittleFS is still the right Phase B backing, but as a
*staging buffer* rather than storage. So:

> Define a `filestore` interface (list / open / read-at-offset / close / sha256 /
> delete). Back it with **LittleFS** now and **SD** when the carrier arrives.
> **The protocol cannot tell the difference.**

This also fixes a Range problem. Today CSV is *generated* from a RAM cursor
(`RecCsvCursor`), so seeking to byte N means regenerating N bytes. Against a real
file, `seek()` is O(1) and Range becomes trivial. **Do not implement Range over the
generated-CSV path** — it would be correct and quadratic.

---

## Module layout (all additive; nothing existing is touched)

```
include/                        src/
  hubproto.h    wire structs      hubproto.cpp    pack/unpack + static_asserts
  hublink.h     STA + fallback    hublink.cpp     join, retry, AP fallback
  hubstream.h   UDP snapshot      hubstream.cpp   5 Hz + 20 Hz fast list
  hubapi.h      /api/v1/*         hubapi.cpp      session, files, ack, time
  filestore.h   file abstraction  filestore_lfs.cpp  (filestore_sd.cpp later)
  session.h     boot_id / anchor  session.cpp     NVS counter, wrap, anchor
```

Untouched: `main.cpp` mode machine, `sniffer`, `obd`, `recorder`, and every existing
route (`/`, `/api/table`, `/api/mode`, `/api/clear`, `/api/rec`, `/api/raw.csv`,
`/api/changes.csv`). Passive modes stay the default; `MODE_SELFTEST` and `MODE_POLL`
keep their deliberate-confirmation gate.

---

## Phase A — no SD required, testable today

**A1 `session.cpp`** — `Preferences` is already used in `main.cpp`, so NVS is in hand.
- `boot_id` = NVS monotonic counter, `++` once at boot.
- `device_id` = low 4 octets of the MAC, little-endian (matches
  `proto.device_id_from_mac`).
- **millis() wrap:** `millis()` rolls at ~49.7 d. On `now < last`, close the session
  and increment `boot_id`. One NVS write per 49 days is nothing against its endurance.
- Anchor: hold `{epoch_ms, uptime_ms, source}` in RAM. **Never fabricate** — absent
  anchor reports `"anchor": null`.

**A2 `hubproto.cpp`** — byte-exact structs.
- Little-endian everywhere; ESP32-S3 is LE natively, so no swapping.
- Layout is naturally aligned at 32 B / 20 B, but add
  `static_assert(sizeof(HubHeader) == 32)` and `== 20` for both records anyway.
  **A silent padding change is exactly how a wire protocol breaks between Python and
  C**, and a static_assert costs nothing.

**A3 `hublink.cpp`** — STA with AP fallback.
- ⚠️ **Do NOT use `WIFI_AP_STA`.** The ESP32 has one radio: AP and STA must share a
  channel, so joining a hub AP on channel 6 silently drags the logger's own AP off
  channel 1. Run **one at a time**.
- Boot → try STA for N seconds → on failure `webuiStart()` exactly as today.
- Credentials go in the gitignored `secrets.h` next to the existing `WIFI_AP_PASS`.
- Periodic retry so the logger joins the hub when the car gets home.

**A4 `hubstream.cpp`** — UDP live stream.
- Reuses the **sniffer's existing per-ID table** — the snapshot model was chosen
  precisely because that table already exists.
- 5 Hz changed-entry snapshot; 20 Hz fast list; `FULL_SNAPSHOT` on (re)connect.
- Record type 2 from `MODE_POLL`: the logger knows the mode+pid it requested.

**A5 `/api/v1/session` + `/api/v1/time`** — no file store needed.
- Constant-time token compare on the POST.

**Desk test:** `MODE_SELFTEST` (TWAI loopback) generates frames → `carhub` listener.
No CAN wiring, no car.

---

## Phase B — needs the file store

**B1 `filestore_lfs.cpp`** — LittleFS backing, rotation by size/time, monotonic index.
**B2 sha256 at close, not on demand.** Computing digests when `/api/v1/files` is
requested would re-read every file on every poll. Compute once at rotation, store in a
sidecar index, serve from there. mbedTLS is already in the Arduino core.
**B3 `/api/v1/files`, `/files/<i>` with Range, `/files/ack`.**
**B4 Tiered retention** — delete acked → oldest unacked Tier A → Tier B/C last resort,
with the `hub/health` usage warning *before* any deletion.
**B5** Swap to `filestore_sd.cpp` when the carrier arrives. No protocol change.

---

## 🔑 CONCURRENCY — decided 2026-09-22

❌ **CORRECTION to this plan's first draft.** It proposed pinning CAN RX to **core 0**.
That is wrong: **Arduino-ESP32 already runs the WiFi/lwIP tasks on core 0 and `loop()`
on core 1**, so CAN RX on core 0 would contend with the network stack rather than
escape it.

✅ **The design (Diego's):**

| Piece | Placement |
|---|---|
| **CAN RX** | dedicated **high-priority task pinned to core 1**, draining TWAI into a ring buffer and nothing else |
| **WebServer** | its own **lower-priority task (or `loop()`) on core 1** |
| **WiFi / lwIP** | core 0, where the Arduino core already puts them — untouched |

Supporting changes:
- **Raise `rx_queue_len`** in the TWAI config so a scheduling hiccup does not drop frames.
- **Stream downloads in small chunks with explicit yields**, so a multi-MB transfer never
  starves the RX task.
- **Serve only closed files** — a file being appended to is never a download source.

🎯 **Goal: `recorderFreezeRaw()` is never needed for sync.** It stays for manual
downloads; automatic sync must not cost frames.

🧪 **Bench test (acceptance):** sustained `/api/v1/files/<i>` download while
`MODE_SELFTEST` generates traffic → **zero dropped frames**. Compare
`recorderRawTotal()` against received count across the transfer.

---

## 💾 Partition table — `firmware/partitions_cardiag_8mb.csv`

Sized around **two OTA app slots**, since the hub spec calls for signed A/B updates
with rollback. Verified to tile 8 MB exactly with no gaps.

| Partition | Offset | Size |
|---|---|---|
| nvs | `0x009000` | 20 KB |
| otadata | `0x00E000` | 8 KB |
| **app0** | `0x010000` | **2.00 MB** |
| **app1** | `0x210000` | **2.00 MB** |
| **spiffs** (LittleFS) | `0x410000` | **3.875 MB** |
| coredump | `0x7F0000` | 64 KB |

2 MB per slot against a ~1.3–1.5 MB build leaves 30–40% headroom. Arduino's
`default_8MB.csv` gives 3.1875 MB per slot and costs the filesystem 2.4 MB for nothing.
⚠ Label is **`spiffs`**, not `littlefs` — Arduino's `LittleFS.begin()` looks for that
label by default.

### 🔴 Retention reality — measured, and it corrects `docs/hardware.md:34`

That line claims *"8 MB onboard flash holds ~28 hrs of Tier B (~80 B/s)"*. Two problems:
8 MB is not available once two OTA slots exist, and **80 B/s is not what the firmware
writes.** Measured against the real 2026-09-08 Civic capture:

| | Rate | 3.565 MB usable holds |
|---|---|---|
| **Measured change-log CSV** | **9,450 B/s** (237 rows/s × 39.9 B) | **6.6 min** |
| Same data, binary ~16 B/frame | 3,789 B/s | 16.4 min |
| Spec "Tier B" decoded @ 1 Hz | 80 B/s | 13.0 h |

🔑 **The 80 B/s figure is DECODED SIGNALS at 1 Hz. The logger does not decode — it
is car-agnostic by design — so what it actually writes is the change log, which is
118× larger.** These are different tiers and the doc conflates them.

⇒ **Conclusion: LittleFS is a STAGING BUFFER, not storage.**
- **Hub present (driving):** files rotate small and the hub pulls them as they close.
  6.6 min of buffer is ample. **Phase B is fully developable and testable this way.**
- **Hub absent:** 6.6 minutes, then retention starts deleting. That **breaks the
  governing rule that the logger is standalone.**
- ⇒ **SD is genuinely on the critical path, exactly as the project notes have said.**
  LittleFS buys bench validation of the file-store and sync paths — it is not a
  substitute for the carrier board.

📌 **Tier A (raw ring) stays RAM-only until SD exists.** At ~1000 fps × 16 B it is
~16 kB/s; internal flash would hold under 4 minutes and burn write cycles doing it.
It remains the PSRAM pre-trigger buffer described in `recorder.h`, committed only on a
trigger, and gets a filesystem home when the carrier lands.

## Protocol v1 friction, found by reading rather than building

Nothing here needs a v1 revision yet. Recording it so the list is honest:

1. **Range against generated CSV is quadratic.** Not a protocol flaw — it is why the
   file store must be real files. Flagged above.
2. **`sha256` in the `/files` listing** implies per-file digests are cheap to obtain.
   On an MCU they are only cheap if computed at close. Worth making explicit in the
   protocol doc as an implementation note.
3. **`record_len` is fixed at 20 for both record types.** Type 2's 10-byte payload is
   generous for Mode 01 and tight for Mode 22. `TRUNCATED` covers it and UDP is
   display-only, so this is fine — but it is the field most likely to want v2.
4. **`uptime_ms` and record `ms` are both u32** and both derive from `millis()`. The
   wrap rule (§3.5) handles it, but the firmware must apply it to *both* consistently.

⇒ **No v1 changes proposed.** Revisit after the board actually runs it.

---

## Preflight status — 2026-09-22

| Check | Result |
|---|---|
| Disk | 🔴 **3.79 GB free** — under the 5 GB floor. **Do not start a build.** |
| PlatformIO | ✅ Core 6.1.19 at `~\.platformio\penv\Scripts\pio.exe` (not on PATH) |
| git auth | ✅ `gh` as `dturru` |

Cleared 0.71 GB of regenerable caches. Remaining safe target: Windows Update cache
(0.97 GB, needs elevation). Real lever: `~\Downloads` at 8.46 GB.

# Hardware — parts and wiring

Prices and stock verified live 2026-08-20. **Re-verify at cart** — availability moves.

## Order now (Phase 0–2)

| # | Part | Why | Price | Source |
|---|---|---|---|---|
| 1 | **ESP32-CAN-X2** (Autosport Labs) | ESP32-S3-WROOM-1-N8R8, dual CAN, **6–20 V nominal / 40 V max automotive input** | **$54.95**, in stock | [autosportlabs.com](https://www.autosportlabs.com/product/esp32-can-x2-dual-can-bus-automotive-grade-development-board/) · [wiki](https://wiki.autosportlabs.com/ESP32-CAN-X2) |
| 2 | **OBD-II male pigtail**, 16-pin to bare wire | Taps the port without cutting anything | ~$10–15, verify | [CERRXIAN 5 ft](https://www.amazon.com/CERRXIAN-OBD2-OBDII-16Pin-Connector/dp/B0CXHLG31Q) · [XMSJSIY 1 m](https://www.amazon.com/XMSJSIY-Extension-Pigtail-Connector-Diagnostic/dp/B0CSK7FRG6) |
| 3 | Inline fuse holder + 500 mA fuse | Protects **the car** from your device | few $ | any auto parts store |

**Roughly $70–75 all in.**

### Why this board is worth 5× a bare ESP32

Its **automotive-grade 40 V-tolerant input removes the entire protection-and-buck subsystem from Phase 1**. No custom PCB, no TVS selection, no reverse-battery FET, no buck to bring up. That is the decoupling this project needs — software and analysis work proceeds while board-level power design stays a separate track.

Bonus: **8 MB PSRAM** is a large Tier-A ring buffer, and **dual CAN** covers a future BMW needing PT-CAN and K-CAN simultaneously.

### 🚫 TRAP: termination resistors ship ENABLED

Per the vendor wiki, CAN termination is **on by default** and is disabled by *scratching copper traces* on breakable jumpers.

A vehicle HS-CAN bus is already terminated at both ends (120 Ω each → 60 Ω effective). Adding a third 120 Ω in parallel drops the bus to ~40 Ω, below the minimum load ISO 11898-2 transceivers are specified to drive. It will often appear to work, which is worse than failing — it produces marginal, intermittent errors you will chase for weeks.

- **Bench (Phase 0):** leave termination ON. Two nodes with 120 Ω each is exactly correct for a standalone bus.
- **Before the car (Phase 1):** cut the termination jumper. Do this once, deliberately, before the first plug-in.

## Deferred — do not order yet

| Part | When it becomes necessary |
|---|---|
| microSD breakout | **Phase 2 — and it is on the CRITICAL PATH, not deferred.** Internal flash holds **minutes**, not hours, of what the logger actually writes. See §Storage budget below. |
| DS3231 RTC | Phase 4. Until the device deep-sleeps for days, NTP over home WiFi is sufficient. |
| Hall current sensor / shunt + 24-bit ADC | Separate subsystem. See the vault note. |
| Custom PCB / LM5117-class supply | Only after Phase 2 measures the real power, thermal, and data-rate requirements. Designing it now means designing against guesses. |

## 💾 Storage budget — measured 2026-09-22

> ❌ **Correction.** This file previously claimed *"8 MB onboard flash holds ~28 hrs of
> Tier B (~80 B/s)"*. Both halves were wrong: 8 MB is not available once the partition
> table carries two OTA slots, and **80 B/s is not what the logger writes.**
>
> 80 B/s is **~20 DECODED signals at 1 Hz**. The logger is car-agnostic by design — it
> ships frames and never decodes — so it cannot produce that stream. The doc was costing
> a tier against a rate belonging to a different tier.

**Available filesystem: 3.875 MB LittleFS (≈ 3.565 MB usable after LittleFS overhead)**,
per `firmware/partitions_cardiag_8mb.csv` — 2 MB × 2 OTA app slots leaves the rest.

### 🔑 TIER vs KIND — the canonical table

> ⚠ **This table is the single source of truth for tier naming.** It is repeated
> nowhere else; `firmware/include/filestore.h`, `carhub/docs/protocol.md` and the
> `files.tier` column all defer to it.
>
> **TIER = the retention class** (what gets deleted first). **KIND = what is in the
> file** (how it parses). They are not the same thing, and a tier can hold more than
> one kind — **Tier A holds two**, which is exactly why one letter was not enough.
>
> ❌ **Corrections folded in here:** an earlier Phase B draft called the change log
> **"Tier C"** — wrong, Tier C is the trip bookends. `recorder.h` called it
> **"tier 1"** — also wrong. The change log is *deduplicated raw frames*, so it has
> always been **Tier A**. This file never assigned it a letter at all, which is how
> the drift went unnoticed.

| Tier | Kind | What it is | Rate | 3.565 MB holds | Phase B |
|---|---|---|---|---|---|
| **A** | `raw` | every frame, pre-trigger ring | ~16 kB/s | **RAM ONLY** — under 4 min on flash, and it would burn write cycles doing it. PSRAM until SD exists | ✗ |
| **A** | `changes` | frame appended when a non-heartbeat byte moves | **9,450 B/s** measured · 3,789 B/s if binary | **7 min** · 16 min | ✅ CSV |
| **B** | `snapshot` | latest frame per ID, **1 Hz**, no decoding | 186 B/s @ 14 IDs · 537 B/s @ 41 | **5.6 h** · 116 min | ✅ binary `CDGS` |
| **C** | `bookend` | trip bookends — DTCs, Mode 06 at trip start/end | bytes per trip | effectively unbounded | ✗ not implemented |
| ~~"Tier B" 80 B/s~~ | — | decoded signals @ 1 Hz | — | **the logger never writes this** | — |

**On disk:** `NNNNNN_TK_BBBBBBBB.{part,log,meta}` — `T` is the tier letter, `K` the kind
letter (`r c s b`). The **tier is derived from the kind**, never stored twice: a file
whose two letters disagree was written by a version with the mapping wrong, and the
kind is the fact.

**Deletion order** — by freeable bytes × replaceability, not by sentiment:
**acked → unacked A → unacked B → unacked C.** C is last and it is mostly a formality:
bookends are bytes per trip, so deleting one frees nothing. Tier A is additionally
**capped at `FS_TIER_A_MAX_PCT` (40%)**, because ordering alone cannot protect Tier B
from something that outruns it fifty to one.

Change-log rate is measured from `analysis/captures_2026-09-08_civic_stimulus1.csv`:
33,911 rows / 143.2 s = **237 rows/s at 39.9 B/row**.

### The 1 Hz snapshot log — adopted for Phase B

Writes the **latest frame per CAN ID once per second**. No decoding, so it stays
car-agnostic, and it reuses the per-ID table the sniffer already maintains (the same
table that feeds the UDP snapshot stream at 5 Hz).

**On-disk format — per-second block**, which is 33% cheaper than repeating a full
protocol record per ID:

```
block:  u32 ms                      (one timestamp for the whole second)
        u16 count
        count x { u32 can_id ; u8 dlc ; u8 data[8] }   = 13 B per ID
```

`changed` is deliberately absent: it is a change-log concept and means nothing in a
snapshot.

⚠ **This IS fixed-rate sampling, which `recorder.h` argues against** — *"fixed-rate
sampling would alias away exactly the transients this project exists to catch."* That
principle is not abandoned, it is **tiered**:

| | keeps | loses | retention |
|---|---|---|---|
| change log | every transient | — | short |
| snapshot log | trends at the 1 Hz the baselining layer needs | transients between samples | long |

🔑 **Retention deletes Tier A (the change log) BEFORE Tier B (the snapshot log).** Under pressure the
device keeps hours of trend data and loses sub-second detail — the right trade for
self-baselining, which compares operating points across months.

⇒ **This is what makes the standalone-logger guarantee hold without the hub**: hours of
1 Hz data instead of 7 minutes.

🔴 **UNVERIFIED — the dominant variable is the ID count.** The only measurement so far
is **14 IDs**, from a changes-only stimulus capture. Retention swings hard with it:

| distinct IDs | snapshot rate | holds |
|---|---|---|
| 14 (measured) | 186 B/s | 5.6 h |
| 41 (assumed) | 537 B/s | 116 min |
| 60 | 784 B/s | 79 min |

**A 60-second `MODE_LISTEN` capture settles this.** Do it before sizing anything else.

### Why SD is still on the critical path

With the hub present, LittleFS is a **staging buffer** — files rotate small and get
pulled as they close, so minutes of buffer suffice and Phase B is fully developable.
With the hub absent, the snapshot log buys hours rather than minutes, but the change
log still expires in single-digit minutes. **The logger being genuinely standalone
requires SD.**

**No USB-CAN adapter needed.** The dual-CAN board self-tests: wire CAN1 to CAN2 and it is its own bench bus.

## Wiring — OBD-II J1962 (Type A, 12 V)

| OBD pin | Signal | Goes to |
|---|---|---|
| 16 | +12 V (**always hot**) | inline 500 mA fuse → **SV2 VIN (pins 19-20, "12 Vin")** — *not* X1 pin 1 |
| **5** | **Signal ground** | X1 pin 4 (GND) |
| ~~4~~ | ~~Chassis ground~~ | **do NOT also connect** — see note below |
| 6 | CAN-H | CAN1 H |
| 14 | CAN-L | CAN1 L |

Standard pigtail color code: **6 = green, 14 = brown/white, 16 = green/white, 4 = orange, 5 = yellow.** Ring the wires out with a meter before trusting the colors — vendors are inconsistent.

HS-CAN on this car runs **500 kbit/s**.

⚠️ Pin 16 is unswitched. The device is powered whether the car runs or not — which is why sleep eventually matters, and why for Phase 1–3 you unplug it when parking.

## Constraints that drive the design

- **Sleep draw target < 1 mA.** Civic battery is ~45–60 Ah; 50 mA kills it in roughly two weeks parked. The regulator's own quiescent current counts, not just the MCU's.
- **Read-only while moving.** No UDS writes, no diagnostic sessions.
- **Thermal:** a parked Texas cabin reaches 60–70 °C. No electrolytics, no LiPo, 85–105 °C rated parts.
- **Mechanical:** the port sits at the driver's right knee and gets kicked. Low profile, or tuck the box and run a short pigtail.
- **ESP32 TWAI RX queue is shallow.** Drain it in the ISR into your own ring buffer or you silently drop frames — which destroys exactly the intermittent-fault case Tier A exists for.

### Board side — X1 / X2 (board side) connector pinout

Confirmed against the vendor wiki 2026-09-07. **All four wires land on ONE connector — no soldering to the PCB.**

| X1 pin | Signal |
|---|---|
| 1 | +12V_AUX |
| 2 | CAN1H |
| 3 | CAN1L |
| 4 | GND |

X2 is identical but CAN2H/CAN2L. **Use X1 only** — CAN2 is the MCP2515 and has no firmware.

### ❗ Ground: use pin 5 ONLY, do not bridge 4 and 5

OBD pin 4 is **chassis** ground, pin 5 is **signal** ground. Reference pin 5 — it is what the ECUs' own CAN
transceivers use, which is what keeps you inside the transceiver **common-mode range**. Do not bond 4 and 5 at
your connector: they are already bonded somewhere in the vehicle, and tying them at the OBD port routes any
chassis-vs-signal potential difference through your thin fused pigtail. If `bus_err` climbs on an otherwise
healthy bus, pin 4 is the fallback to try.

### ✅ RESOLVED: power the board from SV2 VIN, not X1 pin 1

The board has **three** power inputs, and only one of them has a 12 V ceiling:

| Input | Wiki rating | OK at a running car's ~14.4 V? |
|---|---|---|
| USB-C socket | 6-20 V nominal, 40 V max | yes |
| **VIN — header SV2, pins 19-20, "12 Vin"** | **6-20 V nominal, 40 V max** | **yes** |
| X1 / X2 pin 1 (+12V_AUX) | "6-12V Power Supply" | **no** |

⇒ **Run OBD pin 16 through the 500 mA slow-blow fuse into SV2 VIN.** X1 then carries only CAN-H, CAN-L and
ground.

The vendor docs are internally inconsistent about X1 pin 1 — the pigtail is described as supplying "power +
CAN data" into X1, which would mean feeding a running car into a pin labelled 6-12 V. That argument does not
need settling: **SV2 VIN is explicitly rated to 40 V, so use the input whose rating is not in dispute.**

⚠️ **Confirm the silkscreen at SV2 reads VIN / 12Vin before connecting.** This pin numbering comes from the
vendor wiki, not from the board in hand, and 12 V into the wrong header pin destroys it.

⚠️ **Pin 16 is always hot**, so the board runs with the key out. The `<1 mA` sleep budget is still unsolved
— unplug after a session, and treat ignition-switched power as the real answer.

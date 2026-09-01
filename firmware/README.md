# firmware — Phase 0

Bring-up only. Two modes, no external libraries.

## Run it

Self-test first. Nothing connected to the board except USB:

```bash
cd firmware
pio run -t upload -t monitor
```

Expect a frame echoed back roughly once a second, plus a stats line:

```
[   3021] STD 0x100  dlc=4  00 00 00 05
-- 1 fps | 5 total | 1 unique IDs | rx_q=0 missed=0 overrun=0 bus_err=0 tx_err=0
```

If that works, the toolchain, the TWAI driver, the timing config, and the frame
handling are all proven — with zero wiring involved. That isolation is the whole
point of doing self-test before touching a vehicle.

Then switch to the sniffer by editing `include/config.h`:

```c
#define CARDIAG_MODE MODE_LISTEN
```

## 🚫 Before the first car connection

**Cut the CAN termination jumper.** The board ships with 120 Ω termination
enabled, removable only by scratching the copper trace on the breakable jumper
(vendor wiki). A vehicle bus is already terminated at both ends — 60 Ω effective
— and adding a third 120 Ω drops it to ~40 Ω, below the minimum load ISO 11898-2
transceivers are specified to drive.

It will usually still appear to work, which is worse than failing outright: you
get marginal intermittent errors and chase them for weeks.

On a two-node bench bus, termination is correct and should stay. In the car it
must go.

**Wiring:** OBD pin 6 → CAN-H, pin 14 → CAN-L, pins 4/5 → GND, pin 16 → fuse →
VIN. Ring the pigtail out with a meter; vendor wire colors are inconsistent.

`MODE_LISTEN` uses `TWAI_MODE_LISTEN_ONLY`, in which the controller never
transmits and never emits ACK bits. It is provably passive — the correct way to
meet a live vehicle bus for the first time.

## Phase 1a — `MODE_POLL` (draft, not yet run)

**This is the first mode that transmits.** Both Phase 0 modes are provably
passive; this one is not. Do not reach for it until self-test has passed on the
bench and `MODE_LISTEN` has shown real traffic on the car — that sequencing is
the entire reason the passive modes exist.

Sniffing shows you what the car broadcasts. It does not show fuel trims, oxygen
sensor voltages, or misfire counters, because those are internal to the ECU and
are never put on the bus. Getting them means asking, and asking means sending.

```c
#define CARDIAG_MODE MODE_POLL
```

What it does on boot:

1. Queries PID `0x00` / `0x20` / `0x40` and decodes the bitmap of what **this
   specific car** supports. Nothing is assumed to exist.
2. Prints the supported subset it will poll, and separately names the PIDs it
   wanted but the car does not support — so a missing signal shows up as a
   stated gap rather than a silently absent column later.
3. Sweeps that list once a second and prints decoded values.

```
[  4021]  LOAD 18.4%  COOLANT 89.0C  STFT 1.6%  LTFT 4.7%  RPM 812.0rpm
-- 51 req | 51 ok | 0 timeout | 0 malformed | 0 multiframe | last 14 ms | ECUs 7E8
```

### Reading it

- `timeout` climbing on every PID → not connected, key not in accessory, or the
  bus is asleep. `MODE_LISTEN` should show traffic before this mode can work.
- `timeout` on *some* PIDs only → normal-ish; that PID is advertised but slow or
  unanswered. Worth noting, not worth fixing yet.
- More than one ECU listed → expected. A functional request goes to every
  emissions module and any of them may answer.
- `MULTIFRAME` on a signal → the reply did not fit in one frame. Decoding those
  needs flow control, which is deliberately not implemented here (it is Mode 06
  and VIN territory). It is reported rather than silently dropped.

### The bench test worth doing early

`OBD_MULTI_PID_PER_REQUEST` in `config.h` is set to **1** and the current draft
only implements single-PID requests. The standard allows six per request, and
that is the difference between roughly 30 and 180 samples per second — it sets
the ceiling on how much Tier B can ever hold.

Not every ECU honours multi-PID requests, and one that refuses looks a lot like
a dead bus. Test it deliberately, on a parked car, and write the answer into
`docs/hardware.md` before building anything that depends on the faster number.

### Deliberately not here

Flash storage, WiFi, time sync, upload, Mode 06, ISO-TP multi-frame, sleep. Each
belongs to a later phase. This draft exists to answer one question: **do this
car's ECUs answer the way the spec says they will?**

> Written 2026-08-26, before the board arrived. Never compiled against hardware,
> never run against a vehicle.

## Reading the sniffer output

On a running 2012 Civic expect a busy bus — hundreds to low thousands of frames
per second, and on the order of 20–40 distinct standard IDs.

- `fps` near zero with `bus_err` climbing → wrong bitrate, or CAN-H/CAN-L swapped.
- `fps` exactly zero, no errors at all → not actually connected, or the bus is
  asleep. Turn the key to accessory.
- `missed` or `overrun` climbing → the RX queue is overflowing. Expected on a
  busy bus with this naive drain loop. It is a Phase 2 problem (ISR drain into a
  ring buffer), and the counter existing is the point: dropped frames are
  visible rather than silent.

## Notes on choices

**Why no CAN2 / MCP2515 ping-pong.** The vendor ships a CAN1↔CAN2 ping-pong
example, but it needs the Longan Labs `mcp_canbus` library, which is **not in the
PlatformIO registry** (verified 2026-08-20 — zero results) and so would have to
come from a raw git URL. Adding a second CAN controller and an unregistered
dependency to a first bring-up is backwards. Self-test proves the same code path
with strictly fewer moving parts, and the car itself is the real two-node test.

**Why pins are hardcoded here.** The vendor Arduino examples use `CAN1_TX`,
`CAN1_RX`, `CS`, and `LED_BUILTIN` from their own Arduino board variant. Those
macros do not exist under PlatformIO with `board = esp32-s3-devkitc-1`, so the
vendor example does not compile as-is. Values in `include/config.h` are taken
from the vendor wiki.

**Why the pioarduino platform.** The official PlatformIO `espressif32` platform
is still on Arduino core 2.x. ESP32-S3 with core 3.x needs the community fork.

**Why PSRAM is off.** Phase 0 doesn't need it and every build flag is a
potential bring-up failure. It gets enabled in Phase 2 for the ring buffer.

**API version.** This uses the ESP-IDF v5.x *legacy* TWAI driver
(`driver/twai.h`, `twai_driver_install`). ESP-IDF v6 introduced a different
handle-based driver (`esp_twai.h`, `twai_new_node_onchip`) — do not mix the two
when reading documentation.

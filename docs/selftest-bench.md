# SELFTEST on a bare ESP32-CAN-X2 — who ACKs, what termination, why bus-off

## What SELFTEST actually puts on the wire

`MODE_SELFTEST` installs the TWAI driver in **`TWAI_MODE_NO_ACK`**
(`firmware/src/main.cpp`, `twaiModeFor()`), and every frame `selfTestTask()`
sends sets **`tx.self = 1`** (self-reception request). It is **not** an internal
loopback: the ESP32 TWAI controller has none. The path is physical:

```
TWAI controller --TX (GPIO7)--> CAN1 transceiver --> CAN-H / CAN-L at X1
TWAI controller <--RX (GPIO6)-- CAN1 transceiver <-- (the same bus wires)
```

The transceiver drives the bus and, like every CAN transceiver, its receiver
reports the bus level back on RX at the same time. The controller compares
every bit it sends with what it reads back (bit monitoring), and because of the
self-reception request it also delivers its own frame to the RX queue. That is
how SELFTEST "receives" with nothing connected.

## Who ACKs a SELFTEST frame? Nobody — and nobody needs to

A CAN frame has an ACK slot that the transmitter sends recessive; some *other*
node must overwrite it dominant. On a bare X2 there is no other node on CAN1:

| Candidate | ACKs CAN1 frames? |
|---|---|
| The ESP32 TWAI itself | No. A transmitter never ACKs its own frame |
| CAN2 (MCP2515 on SPI) | No. It sits on a **separate** bus (X2), and this firmware never initialises it |
| The X2's termination resistor | No. A resistor is a load, not a node |
| A second node wired to X1 (bench tool, another board) in normal mode | Yes. **Only this** produces an ACK |

`TWAI_MODE_NO_ACK` is the ESP-IDF name for the controller's *self-test mode*,
in which **a missing ACK is treated as success**. So on a bare board every frame
goes out with an empty ACK slot, the controller accepts that, and the transmit
error counter (TEC) does **not** move from it.

## Termination the bench needs

- **Leave the X2's CAN1 termination ON** (it ships on, and comes off only by
  cutting a trace; see `docs/hardware.md` §TRAP). On a bare board that single
  120 Ω is the bus load: it pulls the bus back from dominant to recessive at
  every edge. On a two-node bench, both ends terminated (2 × 120 Ω → 60 Ω) is
  correct.
- **A board whose jumper has been cut for the car** has no load at all on a
  bare bench. The dominant-to-recessive edge then depends on the transceiver's
  own input resistance and stray capacitance, and bit errors become likely.
  (Standard CAN physical-layer reasoning, ⚪ not measured on this board.) For
  SELFTEST on such a board, put a 120 Ω across CAN-H/CAN-L at X1.
- Keep the X1 leads short and twisted or remove them. Nothing needs to be
  connected to X1 for SELFTEST.

## The overnight bus-off (bench 4, 2026-10-05; again on a second night)

**What was seen:** about 1 h 55 min in, a burst of **107 bus errors**, `txfail`
climbing at the full ~235 frames/s, the controller in **bus-off**, `tec` reading
128. Nothing recovered it until #31 (`canrecov.h`), which now does.

**Why `tec = 128` is not the no-ACK signature here.** The classic no-ACK case is
a node in *normal* mode with nobody to ACK it. Every frame then takes an ACK
error (TEC +8) until TEC reaches 128 (error-passive). From there, ISO 11898-1's
error-passive ACK exception stops TEC rising, so the node **parks at 128,
retransmitting forever, and never goes bus-off**. This run was different on
both counts:

1. The controller was in **NO_ACK** mode, where a missing ACK is not an error
   at all.
2. It **reached bus-off**, which ACK errors alone cannot do.

What it does still count in NO_ACK are **bit, stuff, form and CRC errors**.
107 of those in one burst is what took TEC past 255. The `128` read at or after
bus-off is not diagnostic on its own.

**Likely cause: ⚪ UNVERIFIED.** Something disturbed the bus wires or the
transceiver for a burst of frames. In rough order of likelihood:

1. **An EMI or ground transient** coupled through the USB ground (laptop
   sleep/wake, charger plug, hub activity) or picked up by open X1 leads.
2. **A load problem:** termination cut or a marginal contact on that board, so
   edges were already slow and a small disturbance tipped them into bit errors.
3. **A supply dip at the transceiver** (USB 5 V sag during a Wi-Fi transmit
   burst or reconnect).

**How to tell:**

- `/api/v1/session` `can_bus` (#31) now logs each recovery with `tec`, `rec`,
  `bus_err` and the time. Line those times up with the host's USB and power
  events in the soak log.
- Run a night with X1 bare and termination confirmed ON, and a night powered
  from a USB power bank instead of the laptop.
- A scope on CAN-H/CAN-L at the next occurrence would settle it.

## Can it happen in the car?

| Mode in the car | Can it go bus-off? |
|---|---|
| `LISTEN` / `SNIFF` | **No.** Listen-only never transmits and never ACKs, so TEC cannot move. This is how the logger normally runs in the car |
| `POLL` (normal mode) | **The no-ACK path, no**: the ECUs ACK every valid frame. **A bit/stuff/form error burst, yes in principle**: EMI, a bad ground (`docs/hardware.md`: reference OBD pin 5), or a **third terminator** if the X2 jumper was not cut, which leaves the bus at ~40 Ω with marginal edges. Less likely on a correctly wired vehicle bus than on a bare bench, and #31 recovers it through the TX gate. A bus-off node goes silent, so it does not disturb the car |
| `SELFTEST` | **Never run it in the car.** It puts synthetic frames on a vehicle bus. It is not persisted across reboots (transmitting modes never are), but nothing else stops an operator selecting it |

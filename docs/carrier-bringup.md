# Carrier board bring-up checklist

Everything that cannot be tested on the ESP32-CAN-X2 because the dev board has
no INH path, no EN/nSTB control and no buck.

> 🚨 **THE ONE THAT MATTERS MOST.** The as-built transceiver is
> **`TCAN1043GDRQ1`** (vault Carrier Board BOM §11.1). Per the TCAN1043xx-Q1
> datasheet the **G has no `tINACTIVE` / sleep-wake-error failsafe** — the A
> variant does, ours does not. INH sits on **OBD pin 16, which is unswitched
> and permanently live**. So *nothing in hardware will ever turn this board
> off*, and a firmware hang with INH asserted drains the car battery. Every
> item under "Failsafe" below is load-bearing for that reason alone.

## Power-on, before mating the dev board

- [ ] Power-in smoke test: 5 V and 3.3 V rails correct with **nothing mated**
- [ ] `VBAT_PROT` clamps as designed (SMCJ24A) before anything sees raw battery
- [ ] Confirm board underside clearance at the 4 mounts — still **UNVERIFIED**,
      and it gates the enclosure boss height

## Failsafe — the three layers

**Layer 1 — hardware task watchdog**
- [ ] Confirm `[wdt] task watchdog armed, 30s` at boot
- [ ] Force a hang (busy-loop without `esp_task_wdt_reset()`), confirm the chip
      **resets** rather than merely logging
- [ ] Confirm the reset reason after a watchdog fire is distinguishable from a
      power cut in the boot banner

**Layer 2 — boot-time check**
- [ ] Boot with the bus quiet: confirm `transceiverRequestSleep()` runs *before*
      the radio starts and reports `SAFE TO CUT POWER NOW`
- [ ] Boot with a leftover `.part` present: confirm it is closed on the way
      through, not left open until the next mode change
- [ ] Boot with the bus **active**: confirm it refuses and the board stays up

**Layer 3 — max-awake backstop**
- [ ] With `CAN_MAX_AWAKE_MS` temporarily shortened, confirm the board sleeps
      after the bus goes quiet **even with a file open**, and that the file is
      closed first (`BACKSTOP: ... closing, then sleeping`)
- [ ] Confirm the closed file still parses and its sha256 verifies

## INH and the real power cut

This is the test the bench can only simulate. On the dev board the firmware
logs the instant the rail *would* die; here it actually does.

- [ ] Scope INH and the 3.3 V rail together. Command go-to-sleep and confirm
      **INH drops 5–50 µs after EN high + nSTB low** (datasheet `tGO_TO_SLEEP`)
- [ ] Confirm the **ordering**: files closed → sleep commanded → INH drops →
      rail collapses. Anything else and the guarantee is void
- [ ] Measure **sleep current at the OBD connector**. Target is the ~30 µA
      class the architecture was chosen for; anything in milliamps means INH
      did not actually gate the buck
- [ ] Bus-wake: confirm traffic on CANH/CANL brings INH back and the board boots
- [ ] Key-off test in the car: drive, key off, wait past the debounce, pull the
      logger and confirm **zero bytes lost** and every file closed with a digest

## Still open from earlier sessions

- [ ] `TERM2` / termination left unpopulated as designed
- [ ] Harness is dupont + tape and **browns out when moved** — every capture
      taken on it is suspect until it is replaced
- [ ] 60 s `MODE_LISTEN` id-count on a real bus — the dominant unverified
      variable, and nothing on a desk can produce it

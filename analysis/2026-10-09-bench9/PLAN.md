# Bench 9 — X2 carrier USB fault, perfboard check, integrated ignition test (2026-10-09)

**STATUS: HALTED at Phase B (17:00).** Carrier SV2 footprint is mirrored (see `results.md`); the ignition add-on is being folded into a carrier respin instead. Phases C/D not run.

Firmware: cardiag master `64fe27b` (#26 merged: leftover `.part` blocks sleep, TCAN1043 pins). Results: `results.md` (timestamped, every reading).

## Rules
- Never `erase_flash` / `-t erase` / `uploadfs`. Normal upload only, with `PYTHONIOENCODING=utf-8`.
- Serial only via `tools/serial_capture.py`, in its own window, flushed to disk. **No PR #36 hotspot watchdog** (false-negative probe, 91 restarts on 10-08); restart the hotspot by hand if needed.
- Crash dumps → `C:\Users\turru\bench-private\cardiag-dumps\`. No secrets/SSIDs in committed logs.
- Meter range changes (mA ↔ µA) only with a jumper across the meter (opening it reboots the board / blows the µA fuse on a wake spike).
- Commit locally; no push without Diego's OK. BMW-specific notes → carhub only.

## Setup
Supply 12.0 V / 300 mA limit. VBAT: + → meter (mA) → OBD 16. IGN: + → toggle → 500 mA fuse → perfboard `IGN`. − → OBD 4/5. USB laptop ↔ X2 (unplugged for sleep readings). Start: pin 16 disconnected, IGN off.

## Phase A — USB enumeration (supply off, USB only)
1. X2 on carrier → does the COM port appear? (Get-PnpDevice + Kernel-PnP events)
2. If not: (a) GPIO19/20 to every carrier net + GND = open; (b) GPIO0/3/45/46 levels at reset vs S3 boot-mode table; (c) back-feed: VBAT_PROT and X2 +12V pin with USB only, > ~0.5 V = back-feed.
3. Diagnose → propose fix → wait for OK before rework.
4. Unfixable today → decide: flash off carrier and continue, or stop.

## Phase B — perfboard (USB unplugged, pin 16 disconnected)
Net names (no perfboard schematic exists; inferred from `docs/hardware.md` §Carrier board wiring, Diego to confirm):
`IGN` = row 2 (fused switched input) · `+12V` = row 6 (diode cathode into X2 +12V) · `IGN_SENSE` = row 9 (NPN collector → IO21, SV1.8, active-LOW) · `GND` = rail · `3V3` = R3 pull-up (10 k) top.
- Off: `IGN`–`GND` not shorted; continuity `+12V` → X2 +12V pin, `IGN_SENSE` → IO21 (SV1.8), `GND` → carrier `GND`, R3 → X2 `3V3`; `3V3`–`GND` not shorted.
- `IGN` on 12 V: X2 boots; `+12V` 11.5–11.7 V; `3V3` = 3.3 V; `IGN_SENSE` < 0.2 V. `IGN` off: X2 off.
- `IGN` 14.4 V: same, `IGN_SENSE` < 0.2 V.

## Phase C — flash `esp32-can-x2-ignition` from `64fe27b`; banner shows the ignition build.

## Phase D — integrated ignition (pin 16 = KL30, IGN switched)
1. IGN on: boot, IO21 ON, logging, normal mode restored (#23).
2. IGN off: files closed → sleep → INH drops → power off; time IGN-off → off.
3. Sleep current: USB out, 60 s, read (µA if < 1 mA). Target < 1 mA.
4. IGN on: wakes, previous files intact, logdrop sane.
5. 5 fast cycles (~20 s on, off until power off) + 1 off during active writing; all files intact.
6. Glitch: IGN off < 1 s → no shutdown, or clean recovery. Record which.

**PASS** = A resolved/worked around · B in range · D1–6 clean · sleep < 1 mA.

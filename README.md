# cardiag

OBD-II / CAN diagnostic logger and analysis platform.

**v1 target vehicle: 2012 Honda Civic (9th gen, FB2).** BMW is v2.

## What this is

Not a code reader. Code readers are solved. This is a **continuous logger plus an analysis layer** — the part nobody sells well.

The premise: a $20 dongle tells you what's wrong *right now*, after it's already wrong. This logs the car continuously, learns what normal looks like *for this specific car*, and flags deviation from its own baseline. Fuel trim drifting only below 60 °C coolant is a story a snapshot can't tell.

## Scope

**In:**
- Standard OBD-II Modes 01/02/03/06/07/09 over ISO-TP. No DBC, no reverse engineering required.
- Tiered logging (see `docs/architecture.md`)
- Off-device baselining and anomaly detection

**Out (for now):**
- Writing to the bus. Read-only while the vehicle is moving, full stop.
- ECU flashing / tuning. Different discipline, brick risk, not diagnostics.
- Manufacturer UDS beyond standard modes. Possible later; unbounded.

## Why not opendbc

Checked 2026-08-20. Two independent reasons it doesn't apply:

1. **No coverage.** Honda's entire presence in `commaai/opendbc` is one 5.2 KB file (`acura_ilx_2016_nidec.dbc`) out of 60 DBCs. The 2012 Civic appears nowhere; the earliest Honda anything is a 2015 CR-V. The "Civic 2016+" entries in its supported-cars list are openpilot *control* support on a shared Nidec/Bosch message set, not per-model coverage.
2. **Wrong domain.** opendbc is an ADAS library — steering angle, wheel speeds, cruise state. Fuel trims, O2 voltages, misfire counters, and MAF are not in it and are **not broadcast on the bus at all**. They're ECU-internal, reachable only through OBD-II request/response.

This is good news: standard modes need zero reverse engineering and work on the Civic today.

## Layout

```
firmware/    PlatformIO, ESP32-S3, Arduino framework + native TWAI driver
analysis/    Python — baselining, correlation, anomaly detection
docs/        hardware.md (parts + wiring), architecture.md (design)
data/        local logs, gitignored
```

## Build order

Sleep architecture is deliberately last. It is the hardest thing to debug and it is an
optimization, not a feature — building it first means weeks of wake-behavior debugging
before a single row of data exists. Until then: plug it in to drive, unplug it to park.

- [ ] **Phase 0** — bench. Loop CAN1 to CAN2 on the dev board, send and receive one frame.
- [ ] **Phase 1** — in the car, request RPM (PID 0x0C) and print it. Proves the whole chain.
- [ ] **Phase 2** — full OBD-II client, Tier B logging to flash/SD.
- [ ] **Phase 3** — upload to Supabase, analysis layer.
- [ ] **Phase 4** — sleep architecture.

## Related

- Design notes live in the Obsidian vault: `Projects/CAN-Diagnostics/CAN Diagnostics Overview.md`
- Data pipeline pattern is inherited from the Ventis project.

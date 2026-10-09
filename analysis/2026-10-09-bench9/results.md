# Bench 9 results — 2026-10-09

| Time | Phase/step | Reading / result | By |
|---|---|---|---|
| 16:23:19 | A1 | X2 on carrier, USB only, supply off: **enumerates** (USB Composite `VID_303A&PID_1001`, serial `3C:0F:02:F0:4C:74`, `COM3`, JTAG unit, all Status OK). Same board as the 10-08 soak logger. | PnP query |
| 16:24:44.620 | A1 replug | USB unplugged → device gone | `a1-replug.log` |
| 16:24:52.812 | A1 replug | Replugged → re-enumerates as `COM3` (~8 s incl. Diego's 5 s wait). **10-01 "no enumeration on carrier" does NOT reproduce with USB only.** A2 skipped. Still to check: enumeration with 12 V applied (Phase C/D). | `a1-replug.log` |
| 16:35 (reported) | B1 | `IGN`–`GND`: not shorted ✅ | Diego (meter) |
| 16:35 (reported) | B2 | `+12V` → X2 +12V pin: continuity ✅ | Diego |
| 16:35 (reported) | B3 | `IGN_SENSE` → IO21 (SV1.8): continuity ✅ | Diego |
| 16:35 (reported) | B4 | Perfboard `GND` → carrier `GND`: **NO continuity ❌** | Diego |
| 16:35 (reported) | B5 | R3 → X2 `3V3`: continuity ✅ | Diego |
| 16:38 (reported) | B4a | Perfboard `GND` → X2 `GND`: continuity ✅. X2 `GND` → carrier `GND`: **OPEN ❌** ⇒ fault is X2↔carrier, not the perfboard. X2 GND reaches the carrier ONLY via SV2.16/17/18 (vault carrier BOM header table). | Diego |
| 16:44 (reported) | G2 | X2 `GND` → carrier SV2.16/17/18 socket joints: continuity ✅ (seating OK) | Diego |
| 16:44 (reported) | G3 | Carrier SV2 `GND` joint → OBD pin 4/5 pad: **OPEN ❌** ⇒ carrier-side break between SV2 `GND` and OBD `GND` | Diego |
| 16:52 (reported) | G-root | 🔴 **ROOT CAUSE (Diego, visual): carrier header footprint numbers 1–20 in the OPPOSITE direction to the X2's header.** The vault had this exact risk "CLOSED 09-10" from the NETLIST (`SV2-16/17/18 = GND` etc.), but a netlist cannot show physical pad order. **STOP: no 12 V on the carrier with the X2 mated.** Exact mapping (which header(s), pin n ↔ 21−n?) still to be confirmed by meter. | Diego |
| 16:55 (reported) | G-root | **SV2 only** is mirrored (SV1 OK ⇒ IO38/IO39 TCAN1043 mode pins and IO21 route correctly). Presumed map carrier SV2 pad n ↔ X2 SV2 pin 21−n (not yet metered): carrier `+12V_SW` 19/20 → X2 IO5/IO4 · carrier `+5V` 14/15 → X2 IO8/IO18 · carrier `GND` 16–18 → X2 IO15/IO16/IO17 · X2 `GND` → carrier SD SCK/MISO/MOSI pads. Explains B4/G3. | Diego |
| 17:00 | HALT | Bench 9 halted at Phase B. Carrier never powered at 12 V with this X2 mated today (Diego). Decision: fold the ignition add-on into a carrier respin with the SV2 fix rather than test the perfboard further. Phases C/D not run. Note: 09-30 vault §12.21 (mated = no USB) did NOT reproduce today; SV2 mirror is the likely cause then (X2 +5V/+3V3 outs land on carrier signal pads), but why it differs today is unexplained. | — |
| 16:35 (reported) | B6 | `3V3`–`GND`: not shorted ✅ | Diego |

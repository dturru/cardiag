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
| microSD breakout | Phase 2+. 8 MB onboard flash holds ~28 hrs of Tier B (~80 B/s) — plenty for early work. |
| DS3231 RTC | Phase 4. Until the device deep-sleeps for days, NTP over home WiFi is sufficient. |
| Hall current sensor / shunt + 24-bit ADC | Separate subsystem. See the vault note. |
| Custom PCB / LM5117-class supply | Only after Phase 2 measures the real power, thermal, and data-rate requirements. Designing it now means designing against guesses. |

**No USB-CAN adapter needed.** The dual-CAN board self-tests: wire CAN1 to CAN2 and it is its own bench bus.

## Wiring — OBD-II J1962 (Type A, 12 V)

| OBD pin | Signal | Goes to |
|---|---|---|
| 16 | +12 V (**always hot**) | inline 500 mA fuse → board VIN |
| 4, 5 | Chassis / signal ground | board GND |
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

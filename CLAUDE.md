# cardiag — repo instructions

**This is a PORTFOLIO PROJECT: a LOGGER + ANALYSIS tool, not a "read the codes" app.** Don't rebuild
OBD code-reading; the value is the capture pipeline and what the analysis says about the captures.

Full project history and design rationale live in the vault:
`Documents/Diego_School_Vault/Projects/CAN-Diagnostics/CAN Diagnostics Overview.md`
(anchors: `HARDWARE BRING-UP` · `6.3 kB` · `12,612` · `NO HARDWARE FAILSAFE` · `DASHBOARD` · `TIER vs KIND`)

📌 **Session entry point: `docs/NEXT-SESSION.md`.**

## 🛡 Hardware guardrails — these fire BEFORE any lookup

- ⭐⭐ **TO RESET THE BOARD FROM THE HOST, USE esptool — NOT the serial port.**
  `python <platformio>/packages/tool-esptoolpy/esptool.py --chip esp32s3 --port COM3 --after hard_reset read_mac`
  ❌ **CORRECTED 2026-09-24 by measurement.** This entry used to read *"closing the USB serial port
  resets this board (DTR/RTS → EN/BOOT)"*. **It does not.** Measured three ways on COM3:

  | Method | Result |
  |---|---|
  | `serial_capture.py --reset` (DTR/RTS pulse) | ❌ no reset — uptime kept climbing through 122 s |
  | Closing the USB port | ❌ no reset — uptime 155 s, still climbing |
  | `esptool --after hard_reset` | ✅ next capture opens at uptime 1.993 s with the full banner |

  ⇒ **Anything that must see the boot banner** (`[boot] RESET REASON`, `[fs] EFFECTIVE CAPS`,
  `[fs] mounted`) **must hard-reset via esptool FIRST, then capture.** `setup()` prints it ~2 s after
  boot (`delay(2000)` for USB CDC enumeration), so opening a port "shortly after" a flash is a race —
  `run_soak.ps1` lost it and correctly aborted. → `tools/run_soak.ps1` §3b, which retries 3×.
  ⇒ Still true: **use `tools/serial_capture.py`**; `pio device monitor` is interactive and cannot be scripted.
- **A capture is ONE action, repeated ~5×.** The diff tool ranks on MARGIN, and **no quiet baseline
  exists with the engine running** — a capture mixing actions is unrankable.
- **SD is on the critical path.** Both stores are volatile PSRAM and the log fills in ~9 min.
- 🔴 **12.08 V engine-running ⇒ suspected charging fault. Measure at the BATTERY POSTS.**
  **NEVER spec the buck from this car; V_in nominal stays 14.4 V.**
- **Part-marking traps:** `H` suffix = **HIGH-VOLTAGE, not data rate** · `INH` is **BATTERY-referenced,
  not logic** · IO35/36/37 are exposed but **UNUSABLE** (octal PSRAM) · SD **cannot share** the MCP2515
  SPI bus · **there is NO LM5117 on the carrier**.
- ⭐⭐ **FET substitution test = `PIN 1 MUST BE SOURCE`** (drain is always the SOT-223 tab).
  **MATCH BY PIN NUMBER, NEVER BY A PICTURE** — "tab on the left" is an orientation and reverses
  when the part is rotated.
- 🔑 **Verify wiring from the NETLIST, not screenshots.** Three false alarms have come off images.
- **F30 vs E90:** same board, same pins 6-14-16; only the firmware changes (UDS Mode 22).
  **UNVERIFIED on a real F30.**

## 🔑 Conventions

- **TIER = retention class · KIND = what's in the file.** The change log is **TIER A, not C**.
  Canonical table is `docs/hardware.md` §TIER vs KIND **and nowhere else**.
- **`deployment.profile` has NO default** — a bench run must declare it. The BMW profile caps all
  records `verified: false`.
- 🐛 `pio test -e native` needs `test_build_src = yes` in `platformio.ini`; the symptom of forgetting
  is a bogus `undefined reference`.
- 📡 **Never filter on `logger_ip`.** Windows ICS has no reservation mechanism and the bench address
  has already been four different values. **The `device_id` allowlist is the real one.**
- Uploads need `PYTHONIOENCODING=utf-8`.

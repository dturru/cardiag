# cardiag — repo instructions

**This is a PORTFOLIO PROJECT: a LOGGER + ANALYSIS tool, not a "read the codes" app.** Don't rebuild
OBD code-reading; the value is the capture pipeline and what the analysis says about the captures.

Full project history and design rationale live in the vault:
`Documents/Diego_School_Vault/Projects/CAN-Diagnostics/CAN Diagnostics Overview.md`
(anchors: `HARDWARE BRING-UP` · `6.3 kB` · `12,612` · `NO HARDWARE FAILSAFE` · `DASHBOARD` · `TIER vs KIND`)

📌 **Session entry point: `docs/NEXT-SESSION.md`.**

## 🛡 Hardware guardrails — these fire BEFORE any lookup

- ⭐⭐ **A SERIAL TOOL AT DEFAULT DTR/RTS CAN REBOOT THIS BOARD. ALWAYS USE `tools/serial_capture.py`.**
  The S3's native USB-Serial-JTAG maps DTR/RTS → EN/BOOT, and **pyserial asserts DTR on open and drops
  both on close.** This is not theoretical: it **cost a boot_id (12 → 13) mid-bring-up** and orphaned a
  SELFTEST `.part` whose provenance could not be recovered — which is how the `"synthetic":false` bug
  was found. → `tools/serial_capture.py:48`.
  **`serial_capture.py` is safe *because* it pins `dtr=False, rts=False, dsrdtr=False` BEFORE `open()`.**
  A capture tool that reboots the thing it is observing is not a capture tool.

  **TO RESET ON PURPOSE, USE esptool** — it implements the S3's actual reset sequence:
  `python <platformio>/packages/tool-esptoolpy/esptool.py --chip esp32s3 --port COM3 --after hard_reset read_mac`

  ⚖️ **WHAT IS MEASURED vs WHAT IS NOT** (COM3, 2026-09-24). An earlier edit today over-claimed a blanket
  "closing the port does NOT reset the board". **That was wrong: it was measured using the one tool built
  to suppress the effect.** Scope matters here, so the table says exactly what was covered.

  | Tool / config | Reset? | Note |
  |---|---|---|
  | `serial_capture.py`, open + close (`dsrdtr=False`, DTR/RTS pinned low) | ❌ no — uptime 155 s, still climbing | **Suppression BY DESIGN.** Confirms the tool is non-destructive; says nothing about other tools |
  | `serial_capture.py --reset` (pins DTR low, pulses RTS) | ❌ no — uptime climbed through 122 s | The flag did not work; see below |
  | `esptool --after hard_reset` | ✅ yes | Next capture opens at uptime 1.993 s with the full banner |
  | **raw pyserial at DEFAULTS** (DTR asserted on open, dropped on close) | ⚪ **UNTESTED** | This is the documented incident path — assume it DOES reset |
  | **`pio device monitor`** | ⚪ **UNTESTED** | Assume it resets |
  | PuTTY / Arduino IDE / anything leaving `dsrdtr` default | ⚪ **UNTESTED** | Assume it resets |

  ⇒ **Anything that must see the boot banner** (`[boot] RESET REASON`, `[fs] EFFECTIVE CAPS`,
  `[fs] mounted`) **must hard-reset via esptool FIRST, then capture.** `setup()` prints it ~2 s after
  boot (`delay(2000)` for USB CDC enumeration), so opening a port "shortly after" a flash is a race —
  `run_soak.ps1` lost it and correctly aborted. → `tools/run_soak.ps1` §3b, which retries 3×.
  ⇒ `pio device monitor` is interactive and cannot be scripted — another reason it is not the tool here.
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

## Cloud session conventions (token budget)
Cloud sessions run on a limited credit. Spend it on code, not narration.

- Reports: results only. PR link, CI status, findings with file:line, open questions. No recaps of earlier work, no restating the request, no diff walkthroughs; the PR shows the diff.
- No polling. Rely on PR subscriptions for CI failures and review comments. Never schedule check-ins.
- CI logs: read only on failure, and grep for the error; don't read full logs of green runs.
- Tests: run the relevant subset while iterating; run the full suite once before pushing. Let CI do the rest.
- Edit, don't rewrite: targeted edits, never regenerate whole files.
- Read narrowly: grep for symbols before opening files; don't re-read files already in context.
- Batch: one push per logical change, not per fix.
- No simulated merges of all open PRs unless asked.
- No Socratic questions or "something to think about" prompts. If a decision is mine, state it in one line with your recommendation.
- If a task is ambiguous, ask one short question before starting, not after building.
- Hardware, local files (secrets.env, analysis/, MEMORY.md, the vault) and the Pi are out of scope. Say "needs local" and stop.

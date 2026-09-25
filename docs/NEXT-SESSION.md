# Handoff — next session

## ▶ §0-START HERE — 2026-09-24 overnight soak

📂 **READ `analysis/soak-2026-09-24-2251/SUMMARY.md` AND `DONE` FIRST.**
Launched 22:52:30 on AC, 200 cycles, ETA ~06:30. Repo `eeded7b`, pushed.

- `DONE` says `complete-pass` / `complete-fail` / a `preflight-*` or gate state.
  **No `DONE` = it was killed** — `SUMMARY.md` is still valid and will say
  **PARTIAL** with the cycle count, because `soak.csv` fsyncs every cycle.
- ⚠️ **Unacked-eviction warnings are COUNTED, NOT failures** — nothing acks
  overnight, so retention destroys unacked data and says so. That is the
  "never silent" guarantee working. **The verdict is resets and heap only.**
- ⚠️ **Negative `detect_ms` is EXPECTED** — measured against the RETURN of the
  hotspot-stop call; the board's STA-lost event fires ~1.5 s earlier. Only a
  **sign change or drift** across the run means anything, and SUMMARY.md
  checks for exactly that.
- 🔌 Ran **USB-only, CAN harness DISCONNECTED** (the soak never touches the
  bus; SELFTEST is internal loopback), so a `BROWNOUT` is about the board or
  the cable, not the harness.

### Then, in order

1. **Fill the UNTESTED rows** in `CLAUDE.md` §Hardware guardrails, **one tool
   at a time, reading `boot_id` before and after**: raw pyserial at DEFAULTS,
   then `pio device monitor`. Needs the board free.
2. **Ventis Pi backup** (held all night, deliberately). Pi on the **laptop
   hotspot or direct Ethernet — NEVER Dartmouth Wi-Fi** (client isolation).
   `ls -la ~ ~/ventis` FIRST, copy **both** `~/ventis/` and
   `/home/diegot1466/ventis_data.csv`, **sha256 both ends**, record in
   `carhub/docs/deploy-pi.md`. 🚨 **NO REIMAGE until the hashes match.**
   Run it as uninterruptible work.
3. Then the Pi hub setup → `carhub/docs/deploy-pi.md` §1a, §2.

🏁 **Retention is CLOSED** (§ below). Do not re-open claim 2.

## 🚨 §0-AUDIT — "the firmware does X" claims, checked against the source (2026-09-24)

**One claim in this file was FALSE and cost three sessions.** It said the
firmware printed a reset reason on boot. It did not. `grep -rn
'esp_reset_reason\|ESP_RST_' firmware/src firmware/include` returned **nothing**
until `27a490d`. So every run that recorded the 09-24 reboot as "unexplained"
recorded a thing it had no way to explain — and each session re-inherited the
belief that the evidence had been looked for.

🔑 **A handoff note asserting firmware behaviour is a claim, not a fact. Cite
`file:line` or mark it unverified.** Every such claim in this file is now one of
the three below.

| Claim | Status | Implemented at |
|---|---|---|
| Boot line prints the **reset reason** | ❌ **WAS FALSE** → ✅ true from `27a490d` | `firmware/src/main.cpp` §setup, `[boot] RESET REASON:` |
| Boot line prints a **coredump summary** on PANIC/WDT, then erases it | ✅ new, `esp_core_dump_get_summary()` | `firmware/src/main.cpp` §setup, `[boot] COREDUMP:` |
| Board prints its **effective caps** at mount | ✅ added `c0c2645` | `firmware/src/filestore.cpp` (`EFFECTIVE CAPS`) |
| Boot line prints the **lifetime loss record** | ✅ verified | `firmware/src/filestore.cpp:740` |
| `*** EVICTED UNACKED TIER x ... DATA LOST ***` on unacked eviction | ✅ verified | `firmware/src/filestore.cpp:346` |
| `pending_unacked` on `/api/v1/session` | ✅ verified | `firmware/src/hubapi.cpp:159` |
| WDT fed only when armed (`wdtFeedIfArmed`) | ✅ verified | `firmware/src/filestore.cpp:24`, called `:442`, `:460` |
| Hub prints `[deploy] profile=pi` | ✅ verified | `carhub/carhub/web/__main__.py:124`, `carhub/carhub/ingest/udp_listener.py:241` |
| WDT **timing margin** under a marginal link | ⚪ **UNVERIFIED — reasoned, never measured.** The bench cannot produce a marginal link on demand | — |
| 60 s `MODE_LISTEN` id-count on a real bus | ⚪ **UNVERIFIED** — needs the car | — |

---

## 🏁 VERDICT — retention CLOSED, 2026-09-24 22:18

📂 **READ: `analysis/bench-2026-09-24-2218/SUMMARY.md`.**

**All five claims PASS, 0 NOT EXERCISED**, on a verdict that is now three-valued
so a pass cannot be silence. Confirms `bench-2026-09-24-1901` claim for claim.

| Claim | Verdict |
|---|---|
| 2 — the cap's OWN acked-before-unacked choice | ✅ **PASS at last** (4 runs unexercised). 6 acked evicted t=542.9→954.1, then 3 unacked t=1031.7→1194.5 |
| 3 · 3b · 4 · 5 | ✅ PASS |
| reboot | none — no resets in the window |

`[fs] EFFECTIVE CAPS: tier A max 10% = 406323 B` — asserted by the runner, not
read by a human.

## 🏁 (SUPERSEDED) clean-partition run, 2026-09-24 17:36

📂 **READ: `analysis/bench-2026-09-24-1736/SUMMARY.md`** (then `retention.log`,
`retention.csv`, `serial.log` in that folder).

**`rc=0`, zero failed claims — but that is 2 PASS and 3 NOT EXERCISED, not a
blanket pass.** Nothing was evicted at all: usage only reached 30%, below both
the 90% reclaim threshold and the Tier A cap, so there was no eviction pressure
to observe.

| Claim | Verdict |
|---|---|
| **4 — warn fires on loss at LOW usage** | ✅ **PASS.** warn TRUE at **usage 0%** with 34 lifetime losses and only 16 recorded. **The second arm works on its own; total usage was not carrying it.** This is the one no previous run could reach |
| **5 — loss record survives the partition erase** | ✅ **PASS**, and formally this time — the pre-erase baseline (34) *was* captured, so it is a measurement, not evidence. NVS is a separate partition; wiping LittleFS does not reset it |
| 3 — warn before unacked loss | ⚪ NOT EXERCISED — nothing deleted |
| 3b — loss attributed to a tier | ⚪ NOT EXERCISED — nothing deleted |
| **2 — the cap's OWN acked-before-unacked choice** | ⚪ **NOT EXERCISED, third time.** Tier A reached 918 kB of the 1,625 kB cap |

### 🔴 THE REBOOT QUESTION — did NOT reproduce

No reset occurred in the 20-minute window. The only watchdog lines in
`serial.log` are benign init noise, so **the 2026-09-24 mid-run reboot remains
unexplained** — this run simply did not trigger it. Do not treat that as a
clearance.

⭐ The new boot line worked exactly as intended and is the proof claim 5 rests on:

```
[fs] LIFETIME LOSS RECORD: 34 file(s) of uncollected data destroyed
     (A=14 B=20 C=0); hub has recorded 16.  *** WARN STAYS SET ***
```

### 🐛 Regression found and fixed by this run

`E task_wdt: esp_task_wdt_reset(707): task not found` on every boot. The §2 WDT
feeds run from `enforceRetention()` inside `filestoreBegin()`, which is in
`setup()` — **before** the watchdog is armed. Harmless, but a benign error
printed every boot is what hides a real one later. Now guarded by
`wdtFeedIfArmed()`, which checks `esp_task_wdt_status()` first. Deliberately
not fixed by arming earlier: the late arming is what stops a corrupt-partition
format rebooting into itself.

---

## ▶ 1. FIRST CARDIAG TASK — finish claim 2 (~10 min)

Everything is staged. `captest` now carries **`-DFS_TIER_A_MAX_PCT=10`**, moving
the cap to ~406 kB, which the measured **786 B/s** Tier A rate reaches in about
9 minutes.

```
powershell -ExecutionPolicy Bypass -File toolsun_bench.ps1 -Minutes 15
```

🔑 **Why claim 2 kept missing, and it was not one cause but three:**
1. the partition was Tier B dominant → fixed by erasing spiffs;
2. `fstest` set `FS_SNAPSHOT_PERIOD_MS=20`, accelerating the **wrong tier** →
   fixed by the `captest` env;
3. SELFTEST fills Tier A at **786 B/s**, not the 9,450 B/s measured on a real
   bus, because the change log **deduplicates** and SELFTEST emits a fixed
   14-id profile → fixed by lowering the cap for the test.

⚠️ The board currently on the bench was flashed **before** the 10% flag, so it
still has the 40% cap. The runner reflashes, so just run it.

## ▶ 2. FIRST TASK ONCE THE PI IS PLUGGED IN — Ventis backup, treated as uninterruptible

🚨 **BEFORE ANY REIMAGE.** That Pi *is* the Ventis legacy Pi.

- `ls -la ~ ~/ventis` **first** — an older CSV may sit at
  `/home/diegot1466/ventis_data.csv`, one level up from `~/ventis/`. Copying
  only `~/ventis/` could miss it.
- Copy **both** to the laptop, **verify sha256 on both ends**, record the
  hashes in `carhub/docs/deploy-pi.md`, and only then reimage.
- CSV columns are `timestamp, condition, co2_ppm, temp_c, humidity_pct` —
  **`humidity_pct`, not `humidity_rh`**. → `memory/reference_rpi.md`.

## ▶ 3. Pi migration — 3 of 4 blockers cleared

Blockers 2 and 3 fixed (`sink: mqtt`, network → `10.42.0.x`) and MQTT
credentials generated. The hub now prints `[deploy] profile=pi`. **Left, both
off-laptop:** copy the credentials to the Pi's `secrets.env` (verify by
fingerprint, not by eye) and create the broker user with **`allow_anonymous
false`**. → `carhub/docs/deploy-pi.md` §1a.

## ▶ 4. BMW electrical reference — template ready, awaiting TIS

`carhub/docs/bmw-f30-electrical.md` — **private repo on purpose** (TIS diagrams
are copyrighted; only our own notes and citations go in). Every slot empty,
every row needs `source` + `status`. ⏳ Blocked on Diego's TIS access, and §d is
blocked on his VIN option decode.

⏸ **The WDT timing margin still waits for the car** — the bench cannot produce a
marginal link on demand.

---
## 0. Where the measurements are

⚠️ **These are gitignored and therefore LOCAL TO THIS MACHINE.** `.gitignore`
excludes `*.log` and `*.csv` because vehicle captures are a location history;
these are storage/heap statistics with nothing sensitive in them, but the rule
was not overridden to push them. Copied out of `%TEMP%` (which Windows may
clear) into the repo working tree:

```
cardiag/analysis/retention-2026-09-23/
    retention1.log / .csv   fill to 25%, nothing evicted -- baseline
    retention2.log / .csv   THE TIER A CAP RUN -- found the CLAIM 3 failure
    soak50.log   / .csv     the 50-cycle soak that found the 6.3 kB/cycle leak
cardiag/analysis/retention-2026-09-24/
    retention3.log / .csv   post-fix run. See section 1 for what it did and
                            did not manage to exercise.
```

🔑 **The `.csv` is written only when the run COMPLETES** (in `main()`, after
`report()`), so a killed run leaves the `.log` but no `.csv`. The log is written
live and is the one to trust.

---

## ✅ DONE 2026-09-24 — sections 1, 2, 3 and 4 of the previous handoff

Commits: cardiag `9b5a26b`, `4290d6f`, `93a36c1` · carhub `ac24757`, `ae9b34e`.

| Was | Now |
|---|---|
| §1 Tier A "never silent" | **FIXED** — `filestoreWarn()`, per-tier counters, hub/health, dashboard |
| §1b run 3 ordering | **CLAIM 2 NOW PASSES ON HARDWARE** at the `evictOne()` level → `analysis/retention-2026-09-24/claim2-hardware-2026-09-24.md`. The **cap's own** choice is still unexercised — see below |
| §2 WDT vs long ops | **FIXED** in code; format case proved safe by construction |
| §3 key-off sync delay | **DOCUMENTED** (protocol §2.3.3) + `pending_unacked` on the dashboard |
| §4 MEMORY.md compaction | **DONE** — 24.4 kB → 16.7 kB, every pointer grep-verified first |

### What was verified ON HARDWARE (board on COM3, Wi-Fi `192.168.137.64`)

- `lost_files` / `lost_bytes` (then named `unacked_evicted_*`) are live and **attribution
  is exact**: `deleted_unacked=6` with `{A:4, B:2}` summing to 6.
  The Tier B loss (131,256 B) was **invisible in the old total** — it is the
  irreplaceable class and the single number could not distinguish it.
- `pending_unacked` reports (46 at the time of the check), which is the
  key-off-delay number the dashboard now shows.
- **CLAIM 2 PASSES.** Acking a broad prefix mid-run created the contest runs 1
  and 2 never had: `deleted_acked` then moved 0 → 1 → 2 while
  `deleted_unacked` stayed pinned at 26, having been climbing every ~20 s up to
  that point. Retention takes acked first and leaves unacked alone. Evidence:
  `analysis/retention-2026-09-24/claim2-hardware-2026-09-24.md`.
- `warn` reports true. ⚠️ **Only the total-usage arm was exercised** — the board
  sat at 87-91% the whole time, above the 70% threshold, so the run did NOT
  demonstrate the new "warn even when usage is low" arm. See below.

### 🔴 STILL OPEN — run 3 could not reach the state it needs

The bench partition is **dominated by Tier B** (~3.1 MB of 4.06 MB) and sits at
87-91% usage. Two consequences, and both defeat the test:

1. **The Tier A cap never fires.** Tier A never exceeds ~70 kB against a cap of
   ~1.6 MB (40% of the partition), so `enforceTierACap()` — the function §1b is
   about — is never entered. What evicts is `enforceRetention()`'s 90% rule.
2. **`warn` is already true** from total usage, so the second arm of the fixed
   rule is not under test.

Tier A files also rotate at `FS_FILE_MAX_BYTES` (64 kB) and are then evicted
almost immediately by `evictOne()` step 2, so a *closed* Tier A file exists only
for seconds. `--ack-after N` looks for N closed Tier A files and mostly finds
zero.

**To actually run it, start from a clean partition:**

```
# erase the spiffs partition so LittleFS reformats on mount, then:
pio run -e esp32-can-x2-fstest -t upload --upload-port COM3
# board joins the hotspot; then SELFTEST + change log on:
python tools/serial_capture.py --port COM3 --seconds 40 --delay 3 --gap 3 \
    --send "3" --send "y" --send "l"
python tools/retention_watch.py --host <ip> --seconds 1800 --interval 3 \
    --ack-after 6 --csv analysis/retention-2026-09-2x/retention3.csv
```

On an empty partition Tier A grows toward its cap while total usage stays well
under 70%, which is the state that exercises **both** open questions at once:
the cap's acked-before-unacked choice, and `warn` firing on an unacked eviction
while usage is low.

🔑 **Two environment traps that cost time, both already in MEMORY.md:**
- **`PYTHONIOENCODING=utf-8` is required for `pio ... -t upload`.** Without it
  the upload dies after ~10 minutes on a `UnicodeEncodeError` in cp1252 that
  says nothing about the real problem.
- **Windows Mobile Hotspot switches itself off** when nothing connects. Start it
  (`tools/hotspot.ps1 -Action start`) and reset the board promptly, or the join
  fails with `NO_AP_FOUND` against the correct SSID.

📡 **Bench IP has now been `.57 · .119 · .109 · .51 · .64` — five.** Windows ICS
has no reservation. Never filter on `logger_ip`; the `device_id` allowlist is
the real one.

---

## ✅ RESOLVED 2026-09-24 (later) — the volatile-counter hole is closed

cardiag `5211463` · carhub `a7075bf`. The section below is kept for the
reasoning; the decision and the outcome are here.

**Decision taken:** persist cumulative per-tier counters in NVS, monotonic,
never reset, written only on change. The hub records the delta each sync with a
timestamp and owns the history; the dashboard shows "data lost since X" from the
hub's record. **`warn` derives from `lost_files_total > loss_recorded_files`,
both in NVS**, so key-off cannot erase it.

**Hardware-verified:** boot_id 29 → 30 with `deleted_unacked` 9 → 0 (the old
evidence-losing behaviour) while `lost_files_total` went 9 → **16** — it
survived the reboot *and* kept counting. Persisted again across a second reboot.
Loss ack accepted at 16, an over-ack of 99999 clamped to 16, a backwards ack of
0 held at 16.

Also shipped: **contract tests against a real board** (`tests/fixtures/`,
26 tests). They caught two live bugs on their first run — `retention_summary()`
still reading the pre-rename `unacked_evicted_*` keys, and never passing `warn`
through at all. ⚠️ **Re-capture those fixtures from hardware when the firmware's
JSON changes; never hand-edit them to make a test pass.**

🔴 **Reboot cause from the original observation is STILL UNCONFIRMED** — capture
serial alongside the HTTP poll on the next bench run; ~~the boot line prints the
reset reason~~ — ❌ **FALSE WHEN WRITTEN (see §0-AUDIT); true only from
`27a490d`.** If it was the task watchdog, that independently confirms the §2
fix was needed.

## 🗄 1. (HISTORICAL) THE BIGGEST THING FOUND TODAY — the loss counters were volatile

Run 3's rows read `del a/u = 0/22` at t=8.8 and `0/0` from t≈288: **the board
rebooted and the counters started from zero.** Confirmed in the source —
`filestoreBegin()` memsets `g_st` and restores only `idx` and `ack` from NVS, so
`deletedAcked`, `deletedUnacked` and the new per-tier counters are RAM only.

**This undercuts part of what was just built.** Protocol §2.3 names
`deleted_unacked` as the field to act on, and the new `warn` latches on it — and
**the board power-cycles at every key-off**, because INH removes power when the
bus sleeps. The losing sequence is the *normal* one: retention destroys unacked
data mid-trip → the hub does not poll before key-off (expected, per §2.3.3) →
power drops → next ignition reports a clean `deleted_unacked = 0`. The data is
still gone and nothing says so.

**Deliberately not fixed** — persisting on a path that can fire repeatedly costs
flash wear, so it is a design call, not an obvious yes. Preferred option:
**persist on the 0 → non-zero transition only**, so the durable fact is "data has
been lost since this device was last serviced" rather than the running count.
Full reasoning and the alternatives → `analysis/retention-2026-09-24/claim2-hardware-2026-09-24.md`.

🔴 **Reboot cause UNCONFIRMED.** Either the task watchdog fired during eviction —
the board was on the fstest build flashed *before* the §2 WDT fix, so the
retention path fed nothing, at 87-89% with eviction running constantly, which is
exactly §2's hypothesis — or an unrelated crash. **Capture serial alongside the
HTTP poll next run**; ~~the firmware prints the reset reason on boot and settles
it in one line~~ — ❌ **FALSE WHEN WRITTEN. See §0-AUDIT at the top.** Nothing
logged a reset reason until `27a490d`, so there was no line to read. That is
why this question survived three sessions. It is true **now**.

## 🔬 2. QUEUED FOR THE NEXT BENCH SESSION

**One run, two open questions, and a clean partition answers both.** Both are
blocked for the same reason: the bench partition is Tier B dominant (~3.1 MB of
4.06 MB) and sits at 87-91%, so `enforceTierACap()` is never entered and usage
never drops below the 70% warn threshold.

1. **`enforceTierACap()`'s own acked-before-unacked choice.** Claim 2 is verified
   at the `evictOne()` level, not inside the cap.
2. **The low-usage `warn` arm** — warn set by an unacked eviction while total
   usage is still well under `warn_pct`.

Add to the run: **capture serial alongside the HTTP poll**, to settle whether
the 09-24 mid-run reboot was the task watchdog firing during eviction.

⏸ **The WDT timing margin waits for the car** — the bench cannot produce a
marginal link on demand.

### 2a. ▶ RUN IT — one command, detached

```
powershell -ExecutionPolicy Bypass -File tools
un_bench.ps1
```

Launch it in **its own PowerShell window**. It is self-contained: pre-flight
(sleep/hotspot/port), reads the pre-erase loss baseline, erases the spiffs
partition, flashes the fstest build, captures serial for the whole window while
sending the mode keys, finds the board's IP from that live log, then runs the
watch. `-Minutes 30` by default.

📂 **READ RESULTS FROM `analysis/bench-<yyyy-MM-dd-HHmm>/`:**

| File | What |
|---|---|
| **`SUMMARY.md`** | ⭐ **START HERE** — PASS/FAIL per claim, key numbers, the reset/watchdog lines |
| `retention.log` | full verdicts (RETENTION WATCH banner at the end) |
| `retention.csv` | per-sample, **flushed every sample** — a killed run still leaves usable data |
| `serial.log` | the board's own output, **flushed every line** — the boot banner and reset reason live here |
| `runner.log` · `erase.log` · `flash.log` | what the runner did, and the two steps that can abort it |

**What it is testing:** claim 2 (the cap's OWN acked-before-unacked choice),
claim 4 (warn fires on loss while usage is LOW — the arm no run has reached),
claim 5 (the NVS loss record survives the spiffs erase), and the reboot
question, which is the higher-priority one.

⚠️ **NVS is deliberately NOT erased** — it holds the lifetime loss counters, and
leaving it intact is what makes claim 5 testable.

### 2a-bis. Manual recipe (only if the runner cannot be used)
Per the recipe above. The pass conditions are unchanged:

- acked Tier A files are evicted **first**
- unacked Tier A is touched **only after** the acked ones are gone
- `warn` is set on the **first unacked** eviction *while total usage is still
  below `warn_pct`* — the arm that is still unproven
- `deleted_acked` and `deleted_unacked` move independently and correctly

`retention_watch.py` now judges claim 2 on the **observed eviction sequence**
(was: which counter moved first) and adds **claim 3b**, which asserts the
per-tier counters sum to `deleted_unacked`.

### 2b. (DEFERRED — waits for the car) Measure the watchdog margin
§2's fix is structural: every long loop now feeds the WDT after a unit of
provable progress, and the format case is safe because the watchdog is armed
after `filestoreBegin()`. **What is NOT measured is the margin** — how close a
worst-case download over a marginal link actually gets to 30 s. The bench cannot
produce a marginal link on demand. Options: throttle at the AP, or instrument
`handleFetch()` to record its own worst-case duration and report it on
`/api/v1/session` alongside the heap low-water mark, which is the same trick and
needs no special network.

## 3. Unchanged from before

- 🔴 `cea5bcd` (Altium hardware design) is in neither repo; needs Diego's
  interactive Altium 365 credentials.
- 🔴 Board UNDERSIDE clearance at the 4 mounts is UNVERIFIED and gates boss
  height.
- 🚗 The 60 s `MODE_LISTEN` id-count on a real bus is still the dominant
  unknown and the only thing a desk cannot produce.
- Harness is dupont + tape and **browns out when moved**, so every capture taken
  on it is suspect.
- **`github.com/dturru/Ventis` still exposes the Gmail address** on every commit.

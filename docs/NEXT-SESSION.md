# Handoff — next session

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
| §3 key-off sync delay | **DOCUMENTED** (protocol §2.3.1) + `pending_unacked` on the dashboard |
| §4 MEMORY.md compaction | **DONE** — 24.4 kB → 16.7 kB, every pointer grep-verified first |

### What was verified ON HARDWARE (board on COM3, Wi-Fi `192.168.137.64`)

- `unacked_evicted_files` / `unacked_evicted_bytes` are live and **attribution
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

## 🚨 1. THE BIGGEST THING FOUND TODAY — the loss counters are volatile

Run 3's rows read `del a/u = 0/22` at t=8.8 and `0/0` from t≈288: **the board
rebooted and the counters started from zero.** Confirmed in the source —
`filestoreBegin()` memsets `g_st` and restores only `idx` and `ack` from NVS, so
`deletedAcked`, `deletedUnacked` and the new per-tier counters are RAM only.

**This undercuts part of what was just built.** Protocol §2.3 names
`deleted_unacked` as the field to act on, and the new `warn` latches on it — and
**the board power-cycles at every key-off**, because INH removes power when the
bus sleeps. The losing sequence is the *normal* one: retention destroys unacked
data mid-trip → the hub does not poll before key-off (expected, per §2.3.1) →
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
HTTP poll next run**; the firmware prints the reset reason on boot and settles it
in one line. If it was the watchdog, that independently confirms the §2 fix.

## 2. Remaining work

### 1a. Finish retention run 3 on a clean partition
Per the recipe above. The pass conditions are unchanged:

- acked Tier A files are evicted **first**
- unacked Tier A is touched **only after** the acked ones are gone
- `warn` is set on the **first unacked** eviction *while total usage is still
  below `warn_pct`* — the arm that is still unproven
- `deleted_acked` and `deleted_unacked` move independently and correctly

`retention_watch.py` now judges claim 2 on the **observed eviction sequence**
(was: which counter moved first) and adds **claim 3b**, which asserts the
per-tier counters sum to `deleted_unacked`.

### 1b. Measure the watchdog margin
§2's fix is structural: every long loop now feeds the WDT after a unit of
provable progress, and the format case is safe because the watchdog is armed
after `filestoreBegin()`. **What is NOT measured is the margin** — how close a
worst-case download over a marginal link actually gets to 30 s. The bench cannot
produce a marginal link on demand. Options: throttle at the AP, or instrument
`handleFetch()` to record its own worst-case duration and report it on
`/api/v1/session` alongside the heap low-water mark, which is the same trick and
needs no special network.

## 2. Unchanged from before

- 🔴 `cea5bcd` (Altium hardware design) is in neither repo; needs Diego's
  interactive Altium 365 credentials.
- 🔴 Board UNDERSIDE clearance at the 4 mounts is UNVERIFIED and gates boss
  height.
- 🚗 The 60 s `MODE_LISTEN` id-count on a real bus is still the dominant
  unknown and the only thing a desk cannot produce.
- Harness is dupont + tape and **browns out when moved**, so every capture taken
  on it is suspect.
- **`github.com/dturru/Ventis` still exposes the Gmail address** on every commit.

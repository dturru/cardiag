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
    retention2.log / .csv   THE TIER A CAP RUN -- read the CLAIM 3 verdict here
    soak50.log   / .csv     the 50-cycle soak that found the 6.3 kB/cycle leak
```

**The CLAIM 3 verdict is at the bottom of `retention2.log`**, under a
`RETENTION WATCH` banner. `retention_watch.py` also exits non-zero on FAIL.

🔑 **The `.csv` is written only when the run COMPLETES** (in `main()`, after
`report()`), so a killed run leaves the `.log` but no `.csv`. The log is written
live and is the one to trust.



Written 2026-09-23 with context running short. Everything here is agreed work
that was deliberately NOT started, so it does not get half-done.

## 1. Tier A retention gap — fix to the agreed rule

### ✅ CONFIRMED — retention run 2, 2026-09-23. `retention_watch.py` exited 1.

```
[t= 1249.0] EVICTED #7 tier=A kind=changes bytes=65559 synthetic=True
[t= 1249.0] *** first UNACKED deletion, usage 55%, warn=False ***
[t= 1359.9] EVICTED #8 tier=A kind=changes bytes=65842 synthetic=True
[t= 1458.3] EVICTED #9 tier=A kind=changes bytes=65884 synthetic=True

usage      28% -> 55%          tier A  769,470 -> 1,586,755 B
deleted    acked 0  unacked 3  write err 0   rows dropped 0

CLAIM 3 -- warn before any unacked loss:
  ** FAIL **: an unacked file was deleted at usage 55% with warn=False.
```

**Three unacked Tier A files — 197,285 bytes — were destroyed with `warn` still
false, at 55% usage, 15 points below the 70% threshold.** `deleted_unacked` did
increment, so the loss is *reported*; it is simply never *pre-warned*, which is
exactly the ordering the rule below fixes.

**CLAIM 2 remains NOT EXERCISED** — and honestly so: there were no acked files
left to drop, so going straight to unacked was correct behaviour, not a failure.
That is what run 3 (§1b) is for.

Status: measured in retention run 2, verdict above. The gap is that
`enforceTierACap()` evicts Tier A at 40% of the partition **independently of
total usage**, while `storage.warn` fires at 70% of total — so an unacked Tier
A file can be deleted with `warn` still false.

**The rule to implement** (agreed, not my invention): evicting unacked Tier A at
the 40% cap is allowed, but never silent. Any deletion of unacked data must:

- **(a)** set `warn` **regardless of total usage**
- **(b)** increment **per-tier** `unacked_evicted_bytes` / `unacked_evicted_files`
  counters on `/api/v1/session`
- **(c)** be published to `hub/health` **and** shown on the dashboard

Touches: `filestore.cpp` (`evictOne`, `enforceTierACap`, `FileStoreStats`),
`hubapi.cpp` (session JSON), `carhub/ingest/file_sync.py` (consume + publish),
`carhub/web/templates/hub.html` (surface), plus tests on both sides.

### 1b. Retention run 3 — make `enforceTierACap()` actually choose

Runs 1 and 2 could not test the deletion **order**, only the warn rule. Every
file on the disk was acked through index 3 and every Tier A file created during
the fill was unacked, so at the cap there was no acked-vs-unacked contest —
`enforceTierACap()` only ever looks at Tier A, and it had exactly one kind of
candidate. That is why claim 2 is reported as **not exercised** rather than
passed.

**Run 3 setup:** fill Tier A as before, but **ack some of the Tier A files
mid-run** (sync + `POST /api/v1/files/ack` partway through) and leave later ones
unacked, so that when the cap fires there are both kinds present and the
function has to pick.

**Pass:**
- acked Tier A files are evicted **first**
- unacked Tier A is touched **only after** the acked ones are gone
- `warn` is set on the **first unacked** eviction (the rule from §1)
- `deleted_acked` and `deleted_unacked` move independently and correctly

`tools/retention_watch.py` already records eviction order per file index and the
`warn` state at the first unacked deletion, so it should need no changes — only
a fill script that interleaves acks.

## 2. Watchdog vs long operations — bench test

The 30 s task watchdog is armed in `setup()` and fed from `loop()`. Three
operations can plausibly exceed 30 s or block `loop()` long enough to trip it,
and **all three must feed the WDT**:

- a large `GET /api/v1/files/<index>` download (worst case: the biggest file
  the partition can hold, over a marginal link)
- full-disk eviction (`enforceRetention()` walking many files at once)
- a LittleFS format

Test: run each at worst case with the WDT armed, expect **zero resets**. A
watchdog that fires during a legitimate long operation is worse than no
watchdog — it would reboot mid-download forever.

## 3. Document the key-off sync delay as intended behaviour

The logger closes its files ~3 s after the bus goes quiet and then sleeps, which
is *before* the hub can pull them. So a trip's files sync at the **NEXT
ignition**, not at the end of the trip that produced them.

**Nothing is lost** — `Range` resume plus the ack watermark make it a delay, not
a gap — but it is surprising if undocumented, and someone will eventually read
it as a bug.

- Add to `carhub/docs/protocol.md` §2 as stated behaviour, with the reasoning
- Surface on the dashboard as **"N files pending from last trip"**, so the delay
  is visible rather than inferred

## 4. MEMORY.md compaction

`MEMORY.md` is ~23 KB against a ~24.4 KB hard load cap. Compact to under ~17 KB.

🔑 **Verify each pointer target actually holds the detail BEFORE cutting.** The
file's own trim rule says compaction without that check is deletion, and it
records six bad anchors found so far.

Most of this session's detail is already in vault
`Projects/CAN-Diagnostics/CAN Diagnostics Overview.md` §HARDWARE BRING-UP and
§DASHBOARD — grep-verified at the time — but re-verify rather than trusting
this sentence.

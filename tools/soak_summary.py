#!/usr/bin/env python3
"""Write SUMMARY.md for a soak run FROM ITS CSV, so a killed run still gets one.

🛑 This is the half of the uninterruptible-work rule that the old soak was
missing. soak_wifi.py's own report() only runs if the soak reaches the end; an
overnight run cut at 3am produced no verdict at all. This reads the per-cycle
CSV -- which is flushed every cycle -- so a partial run gets a real summary
saying exactly how far it got and what the data so far shows.

It judges the two things the soak exists to answer:

  HEAP FLAT     judged PER BOOT SEGMENT. The leak signal is the trend of
                current free heap and of the largest free block within each
                segment. min-free-heap is reported as HEADROOM only.

                ❌ An earlier version claimed min free heap "only ever falls
                and is NOT restored by a reboot". That is false: it is
                esp_get_minimum_free_heap_size(), a PER-BOOT low-water mark
                that starts again at every reset. Comparing it across a reboot
                compares two different boots.
  ZERO REBOOTS  and, for each one, WHY. "3 reboots" is not a finding.
                "3 BROWNOUTs" (the supply sagged) and "3 TASK_WDTs" (our code
                hung) are different findings with different fixes.
  LOOP LATENCY  FAIL if any cycle's loop() max exceeds LOOP_FAIL_US (1 s),
                naming the cycle and the stage.
  BUS-IDLE      FAIL on ANY "[fs] bus idle ... closed all files" in SELFTEST.
                SELFTEST's bus is the board's own loopback and never goes
                quiet, so a key-off close there is always a false one.
  PANICS        FAIL on any panic/assert line the harness counted.
  CAN DROPS     FAIL on any frame lost before the file: TWAI RX queue full
                (rx_missed), RX FIFO overrun, change-log ring full, sniffer
                id table full (candrops.h). A logger that drops frames under a
                bench load cannot be trusted with a car's.

⭐ THIS IS THE ONLY PLACE A SOAK VERDICT IS DECIDED. `judge()` below is called
by this script (SUMMARY.md + verdict.json), by soak_wifi.py for the VERDICT
line in soak.log, and run_soak.ps1 writes DONE from verdict.json. Three
independent verdicts is how the soak reported PASS three times on runs that
had failed (a 3.74 s loop pass and 1,012 false bus-idle closes on #6 among
them): each copy checked a different subset and none of them agreed.

A check whose input is MISSING fails rather than passes. "Not reported" is
not evidence of health, and a CSV from an older harness should not be able to
earn a PASS the current one would refuse.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import statistics
from pathlib import Path

# Resets that are about the power supply, not our firmware. On the bench with
# the CAN harness disconnected and the cable taped to a direct USB port, a
# BROWNOUT is a real finding about the board or the cable.
SUPPLY = {"BROWNOUT"}
# Resets that are ours, and the actual point of a stability soak.
OURS = {"PANIC", "TASK_WDT", "INT_WDT", "WDT", "CPU_LOCKUP"}
# Host-side or electrical events that are neither a firmware bug nor a supply
# sag. USB is what an esptool reset produces; PWR_GLITCH is the rail, not us.
HOST = {"USB", "JTAG", "SW", "EXT"}


def num(v):
    try:
        return int(float(v))
    except (TypeError, ValueError):
        return None


def slope(ys: list[int]) -> float:
    """Least-squares slope in bytes/cycle. Two points is enough to be honest."""
    n = len(ys)
    if n < 2:
        return 0.0
    xs = list(range(n))
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    den = sum((x - mx) ** 2 for x in xs)
    if den == 0:
        return 0.0
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den


# Free-heap slope threshold, bytes per cycle, applied inside a segment.
HEAP_SLOPE_FAIL = -64.0
MIN_SEGMENT_ROWS = 10

# ⭐ THE LARGEST BLOCK IS NOT JUDGED BY SLOPE. The final bench soak showed its
# real shape: a two-level flip 8 KB apart (HHHH...LLLL...HH...LLLL), stable at
# both levels. A least-squares line through that is steeply negative whenever
# the run happens to end on the low level, though nothing is being lost. What
# fragmentation looks like instead is a STAIRCASE: every step a new low.
#
# So, still strict (any byte below the previous low counts), two checks:
#   1. a new all-time low in the last third of the segment -> FAIL. Early lows
#      are the heap settling; a late one is a floor that is still moving.
#   2. the low level's share of cycles growing in EVERY quarter -> FAIL. A
#      two-level flip whose low level takes over steadily is drifting, even
#      if it never sets a new low.
# "The low level" is every value within LARGEST_LEVEL_TOL of the all-time low:
# half the 8 KB gap, so the two observed levels never merge.
LARGEST_LEVEL_TOL = 4096
LARGEST_QUARTERS = 4


def judge_largest(vals: list[int]) -> dict:
    """Shape check for one segment's largest-free-block series."""
    n = len(vals)
    out = {"low": min(vals) if vals else None, "new_low_at": [],
           "late_new_lows": [], "low_share": [], "fails": []}
    if n < MIN_SEGMENT_ROWS:
        return out
    lo = vals[0]
    for i, v in enumerate(vals[1:], 1):
        if v < lo:
            out["new_low_at"].append(i)
            lo = v
    late_from = n - n // 3
    out["late_new_lows"] = [i for i in out["new_low_at"] if i >= late_from]
    floor = out["low"] + LARGEST_LEVEL_TOL
    q = LARGEST_QUARTERS
    bounds = [round(k * n / q) for k in range(q + 1)]
    out["low_share"] = [
        sum(1 for v in vals[a:b] if v <= floor) / (b - a)
        for a, b in zip(bounds, bounds[1:])]
    if out["late_new_lows"]:
        out["fails"].append(
            f"new all-time low in the last third ({len(out['late_new_lows'])} "
            f"time(s), low {out['low']} B)")
    sh = out["low_share"]
    if all(b > a for a, b in zip(sh, sh[1:])):
        out["fails"].append(
            "low level's share growing every quarter ("
            + " -> ".join(f"{x:.0%}" for x in sh) + ")")
    return out

# One loop() pass over this fails the run. The design budget is ~100 ms per
# call; a second is past any plausible single HTTP request and means something
# in loop() still blocks.
LOOP_FAIL_US = 1_000_000


def segments(rows: list[dict]) -> list[list[dict]]:
    """Split the per-cycle rows at every reboot.

    🔑 THE BOUNDARY. A row with reboot=1 is the cycle IN WHICH the board reset.
    Its heap columns are the last stats line seen in that cycle's window, which
    may be from before the reset or from the fresh boot -- the CSV cannot say
    which. So that row belongs to NEITHER segment, and the next segment starts
    at the FIRST POST-BOOT ROW: the one after it. Putting it on either side
    would splice a value from one boot into a trend of the other.
    """
    segs: list[list[dict]] = [[]]
    for r in rows:
        if num(r.get("reboot")):
            if segs[-1]:
                segs.append([])
            continue
        segs[-1].append(r)
    return [s for s in segs if s]


def judge_heap(rows: list[dict]) -> dict:
    """Leak metric per segment: slope of free heap and of the largest block.

    Returned as data so the tests pin it without parsing Markdown.
    """
    out = {"segments": [], "fails": []}
    for i, seg in enumerate(segments(rows), 1):
        heaps = [h for h in (num(r.get("heap")) for r in seg) if h is not None]
        larges = [v for v in (num(r.get("largest_block")) for r in seg)
                  if v is not None]
        mins = [m for m in (num(r.get("minheap")) for r in seg) if m is not None]
        d = {
            "n": i,
            "rows": len(seg),
            "first_cycle": seg[0].get("cycle"),
            "last_cycle": seg[-1].get("cycle"),
            "heap_first": heaps[0] if heaps else None,
            "heap_last": heaps[-1] if heaps else None,
            "heap_slope": slope(heaps) if len(heaps) >= 2 else None,
            # Shown, not judged: see judge_largest().
            "largest_slope": slope(larges) if len(larges) >= 2 else None,
            "largest": judge_largest(larges),
            # HEADROOM, not a leak signal: the lowest free heap this boot ever
            # reached. Worth knowing how close it came; meaningless across boots.
            "min_free": min(mins) if mins else None,
        }
        out["segments"].append(d)
        if len(heaps) >= MIN_SEGMENT_ROWS and d["heap_slope"] < HEAP_SLOPE_FAIL:
            out["fails"].append(
                f"segment {i} (cycles {d['first_cycle']}-{d['last_cycle']}): "
                f"free heap trending down {d['heap_slope']:+.0f} B/cycle")
        for f in d["largest"]["fails"]:
            out["fails"].append(
                f"segment {i} (cycles {d['first_cycle']}-{d['last_cycle']}): "
                f"largest free block: {f} -- fragmentation")
    return out


def judge_loop(rows: list[dict]) -> dict:
    """Worst loop() pass per cycle; FAIL on any cycle over LOOP_FAIL_US.

    Two columns feed it. `loop_cycle_max_us` is the max of the firmware's
    per-window `loopwin=` values seen in that cycle: the cycle's own worst
    pass. `loop_max_us` is since boot; over the limit it proves SOME pass this
    boot was, so it fails too, attributed to the first cycle it was seen in.
    """
    worst = None          # (us, cycle, stage)
    over: list[str] = []
    boot_flagged = False  # this boot already has a cycle over the limit
    reported = False
    for r in rows:
        cyc = r.get("cycle", "?")
        cm = num(r.get("loop_cycle_max_us"))
        bm = num(r.get("loop_max_us"))
        if cm is not None or bm is not None:
            reported = True
        if cm is not None:
            st = (r.get("loop_cycle_stage") or "").strip() or "?"
            if worst is None or cm > worst[0]:
                worst = (cm, cyc, st)
            if cm > LOOP_FAIL_US:
                over.append(f"cycle {cyc}: {cm / 1000:.0f} ms in {st}")
                boot_flagged = True
        if bm is not None and bm > LOOP_FAIL_US and not boot_flagged:
            # The since-boot max says a pass this boot went over, and no
            # per-cycle value has accounted for it (older firmware without
            # loopwin=, or a pass before the first stats line was read).
            st = (r.get("loop_max_stage") or "").strip() or "?"
            if worst is None or bm > worst[0]:
                worst = (bm, cyc, st)
            over.append(f"cycle {cyc}: since-boot max {bm / 1000:.0f} ms "
                        f"in {st}")
            boot_flagged = True
        if num(r.get("reboot")):
            boot_flagged = False          # a new boot, a new since-boot max
    fails: list[str] = []
    # A loop figure that is missing is a COVERAGE question (judge() below ->
    # INCONCLUSIVE), not a pass and not a fail.
    if over:
        shown = "; ".join(over[:5]) + (f" (+{len(over) - 5} more)"
                                       if len(over) > 5 else "")
        fails.append(f"{len(over)} cycle(s) with a loop() pass over "
                     f"{LOOP_FAIL_US // 1000} ms: {shown}")
    return {"worst": worst, "over": over, "fails": fails}


def judge_fs_sub(rows: list[dict]) -> dict | None:
    """Worst filestore sub-stage over the run. REPORTED, not failed on: the
    loop check above is the verdict; this says where the time went."""
    best = None
    stages: dict[str, int] = {}
    for r in rows:
        us = num(r.get("fs_sub_max_us"))
        if us is None:
            continue
        st = (r.get("fs_sub_stage") or "").strip() or "?"
        stages[st] = stages.get(st, 0) + 1
        if best is None or us > best[0]:
            best = (us, r.get("cycle", "?"), st, num(r.get("fs_pass_us")))
    if best is None:
        return None
    walks = [num(r.get("fs_walks")) for r in rows]
    walks = [w for w in walks if w is not None]
    return {"worst": best, "stage_counts": stages,
            # None = not reported by this firmware. After the in-RAM usage fix
            # only the rare idle resync walks, so any cycle with walks is worth
            # a look; reported, not failed on.
            "walks_total": sum(walks) if walks else None,
            "cycles_with_walks": sum(1 for w in walks if w) if walks else None}


def judge_joins(rows: list[dict]) -> dict | None:
    """Failed joins over the run, from the cumulative joinfail= counter.
    REPORTED, not failed on. With scan-then-join a join is only attempted
    after a scan has seen the hub, so this should be near zero; the baseline
    probe-and-backoff firmware averaged 1.7 per cycle."""
    vals = [(num(r.get("join_fail")), num(r.get("reboot"))) for r in rows]
    if not any(v is not None for v, _ in vals):
        return None
    failed, prev = 0, None
    for v, reboot in vals:
        if v is None:
            continue
        if prev is not None and v >= prev and not reboot:
            failed += v - prev
        elif prev is None or reboot or v < prev:
            failed += 0 if prev is None else v   # counter restarted at boot
        prev = v
    seen = [num(r.get("scan_seen")) for r in rows if num(r.get("scan_seen")) is not None]
    scans = [num(r.get("scans")) for r in rows if num(r.get("scans")) is not None]
    return {"failed_joins": failed, "cycles": len(rows),
            "per_cycle": failed / len(rows) if rows else 0.0,
            "scans_last": scans[-1] if scans else None,
            "seen_last": seen[-1] if seen else None}


# ⭐ COVERAGE. A metric the serial capture could not read on a cycle is not
# evidence of health. The baseline soak lost the counters in 16 of 40 cycles
# to interleaved Serial output and the verdict never noticed. Each required
# metric must be readable on at least COVERAGE_MIN_PCT of cycles; below that
# the verdict is INCONCLUSIVE, never PASS.
COVERAGE_MIN_PCT = 90.0
REQUIRED = {
    "heap": ("heap",),
    "loop": ("loop_cycle_max_us",),
    "fs_sub": ("fs_sub_max_us",),
    "can_drops": ("can_rx_missed", "can_rx_overrun", "can_chg_dropped",
                  "can_id_overflow"),
    # Whether lines were lost at all. Without it a clean coverage figure could
    # hide a queue that dropped the very lines it was counting.
    "log_drops": ("log_dropped",),
    # SELFTEST only; the harness writes it, so it is missing only from a CSV
    # an older harness produced.
    "bus_idle": ("bus_idle_closes",),
}


def logdrop_cycles(rows: list[dict]) -> set:
    """Cycles in which the lossy Serial queue dropped lines (logq.h).

    log_dropped is a since-boot counter, so a cycle dropped lines when it
    reads higher than the last readable value in the same boot segment. The
    first row of a segment counts if it is above zero (drops since boot).
    """
    out = set()
    for seg in segments(rows):
        prev = 0
        for r in seg:
            v = num(r.get("log_dropped"))
            if v is None:
                continue
            if v > prev:
                out.add(r.get("cycle"))
            prev = v
    return out


def judge_logdrop(rows: list[dict]) -> dict | None:
    """logdrop is NOT a failure: the queue is lossy by design and drops only
    console lines, never frames. It is reported, and coverage accounts for it."""
    if not any((r.get("log_dropped") or "").strip() for r in rows):
        return None
    cyc = logdrop_cycles(rows)
    total = sum(max((num(r.get("log_dropped")) or 0) for r in seg)
                for seg in segments(rows))
    return {"total": total, "cycles": sorted(cyc, key=lambda c: num(c) or 0)}


def coverage(rows: list[dict], mode: str = "SELFTEST") -> dict:
    """Per required metric: cycles where every one of its columns was read.

    `in_logdrop_cycles` counts the unreadable ones that fell in a cycle whose
    log queue dropped lines: a gap there may be the queue, not the capture.
    It explains a gap; it never makes one readable.
    """
    out = {}
    dropped = logdrop_cycles(rows)
    for metric, cols in REQUIRED.items():
        if metric == "bus_idle" and mode.upper() != "SELFTEST":
            continue
        bad = [r for r in rows
               if not all((r.get(c) or "").strip() for c in cols)]
        ok = len(rows) - len(bad)
        out[metric] = {"readable": ok, "total": len(rows),
                       "pct": 100.0 * ok / len(rows) if rows else 0.0,
                       "in_logdrop_cycles": sum(
                           1 for r in bad if r.get("cycle") in dropped)}
    return out


def judge(rows: list[dict], *, requested: int = 0,
          mode: str = "SELFTEST") -> dict:
    """THE soak verdict. Everything that reports one calls this."""
    done = len(rows)
    requested = requested or done
    partial = done < requested

    heap = judge_heap(rows)
    loop = judge_loop(rows)
    reboots = [r for r in rows if num(r.get("reboot"))]

    fails: list[str] = []
    if done == 0:
        fails.append("no cycles completed")
    fails += heap["fails"]
    if reboots:
        fails.append(f"{len(reboots)} reboot(s)")
    fails += loop["fails"]

    idle = None
    if rows and all("bus_idle_closes" in r for r in rows):
        idle = sum(num(r.get("bus_idle_closes")) or 0 for r in rows)
        if idle and mode.upper() == "SELFTEST":
            at = [r.get("cycle", "?") for r in rows
                  if num(r.get("bus_idle_closes"))]
            fails.append(f"{idle} bus-idle close(s) in SELFTEST, whose bus "
                         f"never goes quiet -- every one is false (cycles "
                         f"{', '.join(at[:12])}{' ...' if len(at) > 12 else ''})")

    DROP_COLS = {"can_rx_missed": "TWAI RX queue full (rx_missed)",
                 "can_rx_overrun": "TWAI RX FIFO overrun",
                 "can_chg_dropped": "change-log ring full",
                 "can_id_overflow": "sniffer id table full",
                 "can_raw_busy": "raw ring busy (canTask try-lock failed)"}
    drops: dict[str, int] | None = None
    if rows and any((r.get(c) or "").strip() for r in rows for c in DROP_COLS):
        drops = {}
        for col, what in DROP_COLS.items():
            vals = [num(r.get(col)) for r in rows]
            vals = [v for v in vals if v is not None]
            worst = max(vals) if vals else 0
            drops[col] = worst
            if worst:
                first = next(r.get("cycle", "?") for r in rows
                             if (num(r.get(col)) or 0) > 0)
                fails.append(f"CAN drops: {worst} {what} (first at cycle "
                             f"{first})")

    panics = sum(num(r.get("panics")) or 0 for r in rows)
    if panics:
        fails.append(f"{panics} panic/assert line(s)")

    cov = coverage(rows, mode)
    short = {m: c for m, c in cov.items() if c["pct"] < COVERAGE_MIN_PCT}

    # FAIL wins: a check that failed on the cycles it could read is a failure
    # whatever the rest look like. Otherwise, a required metric that could not
    # be read on >= 90 % of cycles means the run cannot say PASS.
    if fails:
        verdict = "FAIL"
    elif short:
        verdict = "INCONCLUSIVE"
    elif partial:
        verdict = "PARTIAL"
    else:
        verdict = "PASS"
    return {"verdict": verdict, "fails": fails, "done": done,
            "requested": requested, "partial": partial, "heap": heap,
            "loop": loop, "reboots": reboots, "bus_idle_closes": idle,
            "panics": panics, "can_drops": drops, "fs_sub": judge_fs_sub(rows),
            "coverage": cov, "coverage_short": sorted(short),
            "joins": judge_joins(rows), "logdrop": judge_logdrop(rows)}


def read_rows(csv_path) -> list[dict]:
    path = Path(csv_path)
    if not path.exists():
        return []
    with path.open(encoding="utf-8", newline="") as fh:
        return list(csv.DictReader(fh))


def verdict_lines(j: dict) -> list[str]:
    """The VERDICT block soak_wifi.py prints into soak.log. Same words as
    SUMMARY.md, from the same judge() result."""
    out = [f"VERDICT: {j['verdict']} ({len(j['fails'])} failed check(s), "
           f"{j['done']}/{j['requested']} cycles)"]
    out += [f"  - {f}" for f in j["fails"]]
    for m in j.get("coverage_short", []):
        c = j["coverage"][m]
        out.append(f"  - coverage: {m} readable on {c['readable']}/"
                   f"{c['total']} cycles ({c['pct']:.0f}% < "
                   f"{COVERAGE_MIN_PCT:.0f}%)")
    return out


EXIT = {"PASS": 0, "FAIL": 1, "PARTIAL": 3, "INCONCLUSIVE": 4}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", required=True)
    ap.add_argument("--log", default=None)
    ap.add_argument("--out", required=True)
    ap.add_argument("--requested", type=int, default=0)
    ap.add_argument("--context", default=None,
                    help="run-context.json -- start time and power state, so "
                         "the morning read has context it did not witness")
    ap.add_argument("--verdict-file", default=None,
                    help="write the verdict as JSON here; run_soak.ps1 takes "
                         "DONE's state from it")
    ap.add_argument("--mode", default="SELFTEST",
                    help="board mode the soak ran in (run_soak.ps1 verifies "
                         "SELFTEST before starting)")
    args = ap.parse_args(argv)

    path = Path(args.csv)
    rows = read_rows(path)
    j = judge(rows, requested=args.requested, mode=args.mode)
    done, requested, partial = j["done"], j["requested"], j["partial"]
    heap, reboots, fails, verdict = j["heap"], j["reboots"], j["fails"], j["verdict"]
    netstack = sum(num(r.get("netstack_12308")) or 0 for r in rows)

    reasons: dict[str, list[str]] = {}
    for r in reboots:
        reasons.setdefault(r.get("reset_reason") or "UNREPORTED",
                           []).append(r.get("cycle", "?"))

    L: list[str] = []
    L.append("# Soak run summary\n")
    L.append(f"**VERDICT: {verdict}** ({len(fails)} failed check(s))\n")
    if partial:
        L.append(f"> ⚠️ **PARTIAL — {done} of {requested} cycles.** The run did "
                 f"not finish, so this is what the completed cycles show, not "
                 f"a verdict on {requested}. Top it up with "
                 f"`run_soak.ps1 -Resume`.\n")
    if done == 0:
        L.append("\n> 🔴 **No cycles completed.** Nothing to judge — check "
                 "`runner.log`, `flash.log` and `soak.log`.\n")

    # ⭐ Whoever reads this at 8am did not watch it start. A BROWNOUT at cycle
    # 140 means one thing on mains and something entirely different if the
    # laptop dropped to battery at 3am.
    if args.context and Path(args.context).exists():
        try:
            ctx = json.loads(Path(args.context).read_text(encoding="utf-8"))
        except (OSError, ValueError):
            ctx = {}
        if ctx:
            L.append("\n## Run context\n")
            L.append("| | |")
            L.append("|---|---|")
            for k, v in ctx.items():
                L.append(f"| **{k.replace('_', ' ')}** | {v} |")
            L.append("")

    L.append(f"- **cycles completed:** {done} of {requested}")
    if j["coverage"]:
        cells = ", ".join(
            f"{m} {c['readable']}/{c['total']} ({c['pct']:.0f}%)"
            + (f", {c['in_logdrop_cycles']} missing in log-drop cycles"
               if c["in_logdrop_cycles"] else "")
            + (" ⚠️" if m in j["coverage_short"] else "")
            for m, c in j["coverage"].items())
        L.append(f"- **coverage (cycles with readable values):** {cells}")
    ld = j["logdrop"]
    L.append("- **serial lines dropped (log queue, not a failure):** " + (
        "not reported" if ld is None else
        f"{ld['total']} in {len(ld['cycles'])} cycle(s)" + (
            f" (cycles {', '.join(str(c) for c in ld['cycles'][:12])}"
            f"{' ...' if len(ld['cycles']) > 12 else ''})"
            if ld["cycles"] else "")))
    if verdict == "INCONCLUSIVE":
        L.append(f"\n> ⚠️ **INCONCLUSIVE, not PASS.** No check failed on the "
                 f"cycles that could be read, but "
                 f"{', '.join(j['coverage_short'])} could not be read on at "
                 f"least {COVERAGE_MIN_PCT:.0f}% of cycles. Check "
                 f"serial-raw.log for garbled or missing `[stats]` lines.\n")
    L.append(f"- **reboots:** {len(reboots)}")
    L.append(f"- **netstack 12308 events:** {netstack}")
    w = j["loop"]["worst"]
    L.append("- **worst loop() pass:** " + (
        f"{w[0] / 1000:.1f} ms at cycle {w[1]} (stage `{w[2]}`)" if w
        else "not reported"))
    fsub = j["fs_sub"]
    if fsub:
        us, cyc, st, pass_us = fsub["worst"]
        counts = ", ".join(f"{k} x{v}" for k, v in
                           sorted(fsub["stage_counts"].items(),
                                  key=lambda kv: -kv[1]))
        L.append(f"- **worst filestore sub-stage:** `{st}` {us / 1000:.1f} ms "
                 f"at cycle {cyc}" + (f" (whole tick {pass_us / 1000:.1f} ms)"
                                      if pass_us else "")
                 + f"; worst stage per cycle: {counts}")
        if fsub.get("walks_total") is not None:
            L.append(f"- **filesystem walks in the tick:** "
                     f"{fsub['walks_total']} over the run, in "
                     f"{fsub['cycles_with_walks']} cycle(s) _(only the idle "
                     f"resync should walk)_")
    else:
        L.append("- **worst filestore sub-stage:** not reported")
    cd = j["can_drops"]
    L.append("- **CAN drops:** " + ("not reported" if cd is None else
             ", ".join(f"{k.removeprefix('can_')}={v}" for k, v in cd.items())))
    jn = j["joins"]
    if jn is not None:
        L.append(f"- **failed joins:** {jn['failed_joins']} over {jn['cycles']} "
                 f"cycles ({jn['per_cycle']:.2f}/cycle; scan-then-join should "
                 f"keep this near 0)" + (
                     f"; scans {jn['scans_last']}, hub seen {jn['seen_last']} "
                     f"(since last boot)" if jn["scans_last"] is not None else ""))
    bi = j["bus_idle_closes"]
    L.append("- **bus-idle closes:** " + ("not counted" if bi is None
                                         else str(bi)))

    # Heap, PER BOOT SEGMENT. A reset restores free heap and restarts the
    # min-free low-water mark, so a trend is only meaningful inside one boot.
    if heap["segments"]:
        L.append("\n## Heap, per boot segment\n")
        L.append("_Leak signal: the trend of free heap inside a segment, and "
                 "the SHAPE of the largest free block (a new all-time low in "
                 "the last third, or the low level's share growing every "
                 "quarter, fails; its slope is shown, not judged). **min free** is headroom — the lowest "
                 "this boot reached — and restarts at every reset, so it is "
                 "never compared across segments. A reboot row belongs to "
                 "neither side; each segment starts at the first post-boot "
                 "row._\n")
        L.append("| Segment | Cycles | Rows | Free heap | Slope | Largest-block slope | Largest low (late new lows) | Low-level share by quarter | Min free (headroom) |")
        L.append("|---|---|---|---|---|---|---|---|---|")
        for d in heap["segments"]:
            hs = "—" if d["heap_slope"] is None else f"{d['heap_slope']:+.1f} B/cyc"
            ls = "—" if d["largest_slope"] is None else f"{d['largest_slope']:+.1f} B/cyc"
            lg = d["largest"]
            low = "—" if lg["low"] is None else (
                f"{lg['low']} ({len(lg['late_new_lows'])})")
            share = " / ".join(f"{x:.0%}" for x in lg["low_share"]) or "—"
            L.append(f"| {d['n']} | {d['first_cycle']}–{d['last_cycle']} | "
                     f"{d['rows']} | {d['heap_first']} → {d['heap_last']} | "
                     f"{hs} | {ls} | {low} | {share} | {d['min_free']} |")
        short = [d for d in heap["segments"] if d["rows"] < MIN_SEGMENT_ROWS]
        if short:
            L.append(f"\n> ℹ️ {len(short)} segment(s) under "
                     f"{MIN_SEGMENT_ROWS} rows: slope shown, not judged.")
        L.append("")

    # ⭐ NEGATIVE detect_ms IS EXPECTED, AND SAYING SO HERE IS THE POINT.
    # detect_ms is measured from the RETURN of Windows' hotspot-stop call, not
    # from the moment the radio actually goes off. The board's STA-lost event
    # fires a median ~1.5 s BEFORE that call returns -- the 50-cycle soak
    # measured the same thing -- so the column is routinely negative and that
    # is a property of the yardstick, not a fault.
    #
    # What WOULD be a finding is the sign flipping or the value drifting across
    # the run, so that is what gets checked instead of the raw sign.
    dets = [float(r["detect_ms"]) for r in rows
            if (r.get("detect_ms") or "").strip()]
    if dets:
        med = statistics.median(dets)
        L.append(f"- **detect_ms:** median {med:+.0f} ms "
                 f"(min {min(dets):+.0f}, max {max(dets):+.0f})")
        L.append("")
        L.append("> ℹ️ **`detect_ms` is measured against the RETURN of the "
                 "hotspot-stop call, not the radio-off instant.** The board's "
                 "STA-lost event fires ~1.5 s before that call returns, so "
                 "**negative values are expected and are not a fault** — the "
                 "50-cycle soak measured the same offset. This is a floor on "
                 "detection latency, not the worst case; a fading AP in a car "
                 "is a different test.")
        if len(dets) >= 10:
            half = len(dets) // 2
            first_med = statistics.median(dets[:half])
            last_med = statistics.median(dets[half:])
            signs = {d < 0 for d in dets}
            if len(signs) > 1:
                L.append(f"> ⚠️ **detect_ms CHANGES SIGN across the run** "
                         f"(first half median {first_med:+.0f} ms, second half "
                         f"{last_med:+.0f} ms). That is the case worth looking "
                         f"at — the offset above is supposed to be stable.")
            elif abs(last_med - first_med) > 1000:
                L.append(f"> ⚠️ **detect_ms DRIFTS**: first half median "
                         f"{first_med:+.0f} ms vs second half {last_med:+.0f} "
                         f"ms. A stable offset should not move like this.")
            else:
                L.append(f"> ✅ Stable across the run (first half median "
                         f"{first_med:+.0f} ms, second half {last_med:+.0f} "
                         f"ms) — no sign change, no drift.")
        L.append("")

    # ⚠️ COUNTED, NEVER FAILED ON. Nothing acks overnight, so retention WILL
    # destroy unacked data and the board WILL say so -- loudly and correctly.
    # That is the "never silent" guarantee working, not a soak failure.
    # Retention losses are not in the verdict (judge() above).
    losses = 0
    if args.log and Path(args.log).exists():
        losses = len(re.findall(r"EVICTED UNACKED TIER",
                                Path(args.log).read_text(encoding="utf-8",
                                                         errors="replace")))
    L.append(f"- **unacked-eviction warnings:** {losses} "
             f"_(expected with nothing acking — counted, NOT a failure. "
             f"Not part of the verdict.)_")

    L.append("\n## Reset classification\n")
    if not reboots:
        L.append("No resets observed. ✅ That is the stability claim.")
    else:
        L.append("| Reason | Count | At cycle(s) |")
        L.append("|---|---|---|")
        for name, at in sorted(reasons.items()):
            shown = ", ".join(at[:12]) + (f" (+{len(at)-12})" if len(at) > 12 else "")
            L.append(f"| `{name}` | {len(at)} | {shown} |")
        supply = sum(len(v) for k, v in reasons.items() if k in SUPPLY)
        ours = sum(len(v) for k, v in reasons.items() if k in OURS)
        unrep = len(reasons.get("UNREPORTED", []))
        L.append("")
        if supply:
            L.append(f"- 🔌 **{supply} BROWNOUT — the SUPPLY sagged, not the "
                     f"firmware.** With the CAN harness disconnected and the "
                     f"cable taped to a direct USB port, this is about the "
                     f"board or the cable. Not a firmware finding.")
        if ours:
            L.append(f"- 🔴 **{ours} PANIC/WDT — these ARE ours** and are the "
                     f"real finding of this run.")
        host = sum(len(v) for k, v in reasons.items() if k in HOST)
        if host:
            L.append(f"- 💻 **{host} USB/JTAG/SW — a HOST-SIDE reset**, not the "
                     f"board misbehaving. Expected if anything touched the "
                     f"port mid-run; unexpected otherwise, and then worth "
                     f"asking what did.")
        if "PWR_GLITCH" in reasons:
            L.append(f"- ⚡ **PWR_GLITCH** — the rail glitched. Same class as "
                     f"BROWNOUT: supply, not firmware.")
        if unrep:
            L.append(f"- ⚪ **{unrep} UNREPORTED** — the board booted without "
                     f"printing `[boot] RESET REASON`, so it is running "
                     f"firmware older than that line. Reflash before trusting "
                     f"any classification here.")

    if fails:
        L.append("\n## Failed checks\n")
        for f in fails:
            L.append(f"- {f}")

    # ⭐ A FAIL MUST ARRIVE WITH ITS EVIDENCE. The firmware prints a coredump
    # summary on any PANIC/WDT boot -- the task that died, its PC and the first
    # frames. Copying those lines in here is the difference between "1 TASK_WDT
    # at cycle 137" and knowing where it died, without reproducing it.
    if args.log and Path(args.log).exists():
        text = Path(args.log).read_text(encoding="utf-8", errors="replace")

        dump = [ln.strip() for ln in text.splitlines()
                if "[boot] COREDUMP" in ln]
        if dump:
            L.append("\n## Coredump evidence\n")
            L.append("From the board's own `[boot] COREDUMP` lines, printed on "
                     "the boot AFTER each crash and then erased so the next "
                     "crash is not masked by a stale dump.\n")
            L.append("```")
            L.extend(dump[:40])
            if len(dump) > 40:
                L.append(f"... (+{len(dump) - 40} more lines in soak.log)")
            L.append("```")
            L.append("\n🔍 Resolve the addresses with:\n")
            L.append("```")
            L.append("xtensa-esp32s3-elf-addr2line -pfiaC -e "
                     ".pio/build/esp32-can-x2/firmware.elf <pc> <bt...>")
            L.append("```")
        elif any(r.get("reset_reason") in OURS for r in reboots):
            L.append("\n## Coredump evidence\n")
            L.append("🔴 **A PANIC/WDT reset was recorded but NO "
                     "`[boot] COREDUMP` line appeared.** Either the board is "
                     "running firmware older than that line, or the dump could "
                     "not be read. The crash is real; the evidence is missing.")

        hits = re.findall(r"^.*(Guru Meditation|abort\(\) was called|"
                          r"StoreProhibited|LoadProhibited).*$",
                          text, re.MULTILINE)
        if hits:
            L.append(f"\n## Other notable serial lines\n\n_{len(hits)} "
                     f"match(es); full text in `soak.log`._")

    L.append("\n---\n")
    L.append(f"per-cycle CSV -> `{path.name}` (flushed every cycle, so this "
             f"summary is trustworthy even for a killed run)")

    Path(args.out).write_text("\n".join(L) + "\n", encoding="utf-8")
    if args.verdict_file:
        Path(args.verdict_file).write_text(json.dumps({
            "verdict": verdict, "fails": fails, "cycles_done": done,
            "cycles_requested": requested, "coverage": j["coverage"],
            "coverage_short": j["coverage_short"]}, indent=2) + "\n",
            encoding="utf-8")
    print(f"SUMMARY: {verdict} -- {done}/{requested} cycles, "
          f"{len(reboots)} reboot(s) -> {args.out}")
    # Non-zero unless PASS, so a caller cannot mistake a FAIL for success.
    return EXIT[verdict]


if __name__ == "__main__":
    raise SystemExit(main())

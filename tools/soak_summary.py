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


# Slope thresholds, bytes per cycle, applied inside a segment. -64 B/cycle is
# the existing free-heap threshold; the largest block uses the same one, since
# a fragmenting heap fails an allocation with free heap to spare.
HEAP_SLOPE_FAIL = -64.0
MIN_SEGMENT_ROWS = 10


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
            "largest_slope": slope(larges) if len(larges) >= 2 else None,
            # HEADROOM, not a leak signal: the lowest free heap this boot ever
            # reached. Worth knowing how close it came; meaningless across boots.
            "min_free": min(mins) if mins else None,
        }
        out["segments"].append(d)
        if len(heaps) >= MIN_SEGMENT_ROWS and d["heap_slope"] < HEAP_SLOPE_FAIL:
            out["fails"].append(
                f"segment {i} (cycles {d['first_cycle']}-{d['last_cycle']}): "
                f"free heap trending down {d['heap_slope']:+.0f} B/cycle")
        if (len(larges) >= MIN_SEGMENT_ROWS
                and d["largest_slope"] < HEAP_SLOPE_FAIL):
            out["fails"].append(
                f"segment {i} (cycles {d['first_cycle']}-{d['last_cycle']}): "
                f"largest free block trending down {d['largest_slope']:+.0f} "
                f"B/cycle -- fragmentation")
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", required=True)
    ap.add_argument("--log", default=None)
    ap.add_argument("--out", required=True)
    ap.add_argument("--requested", type=int, default=0)
    ap.add_argument("--context", default=None,
                    help="run-context.json -- start time and power state, so "
                         "the morning read has context it did not witness")
    args = ap.parse_args(argv)

    path = Path(args.csv)
    rows: list[dict] = []
    if path.exists():
        with path.open(encoding="utf-8", newline="") as fh:
            rows = list(csv.DictReader(fh))

    done = len(rows)
    requested = args.requested or done
    partial = done < requested

    heap = judge_heap(rows)
    reboots = [r for r in rows if num(r.get("reboot"))]
    netstack = sum(num(r.get("netstack_12308")) or 0 for r in rows)

    reasons: dict[str, list[str]] = {}
    for r in reboots:
        reasons.setdefault(r.get("reset_reason") or "UNREPORTED",
                           []).append(r.get("cycle", "?"))

    fails: list[str] = list(heap["fails"])
    if reboots:
        fails.append(f"{len(reboots)} reboot(s)")

    if fails:
        verdict = "FAIL"
    elif partial:
        verdict = "PARTIAL"
    else:
        verdict = "PASS"

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
    L.append(f"- **reboots:** {len(reboots)}")
    L.append(f"- **netstack 12308 events:** {netstack}")

    # Heap, PER BOOT SEGMENT. A reset restores free heap and restarts the
    # min-free low-water mark, so a trend is only meaningful inside one boot.
    if heap["segments"]:
        L.append("\n## Heap, per boot segment\n")
        L.append("_Leak signal: the trend of free heap and of the largest free "
                 "block inside a segment. **min free** is headroom — the lowest "
                 "this boot reached — and restarts at every reset, so it is "
                 "never compared across segments. A reboot row belongs to "
                 "neither side; each segment starts at the first post-boot "
                 "row._\n")
        L.append("| Segment | Cycles | Rows | Free heap | Slope | Largest-block slope | Min free (headroom) |")
        L.append("|---|---|---|---|---|---|---|")
        for d in heap["segments"]:
            hs = "—" if d["heap_slope"] is None else f"{d['heap_slope']:+.1f} B/cyc"
            ls = "—" if d["largest_slope"] is None else f"{d['largest_slope']:+.1f} B/cyc"
            L.append(f"| {d['n']} | {d['first_cycle']}–{d['last_cycle']} | "
                     f"{d['rows']} | {d['heap_first']} → {d['heap_last']} | "
                     f"{hs} | {ls} | {d['min_free']} |")
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
    # THE SOAK VERDICT IS RESETS AND HEAP ONLY.
    losses = 0
    if args.log and Path(args.log).exists():
        losses = len(re.findall(r"EVICTED UNACKED TIER",
                                Path(args.log).read_text(encoding="utf-8",
                                                         errors="replace")))
    L.append(f"- **unacked-eviction warnings:** {losses} "
             f"_(expected with nothing acking — counted, NOT a failure. "
             f"The verdict above is resets and heap only.)_")

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
    print(f"SUMMARY: {verdict} -- {done}/{requested} cycles, "
          f"{len(reboots)} reboot(s) -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

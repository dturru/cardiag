"""Check the SELFTEST profile still reproduces the capture it was derived from.

selftest_profile.h claims its periods come from the 2026-09-08 Civic capture.
That claim is checkable without hardware: simulate selfTestTick()'s arithmetic
at the 1 kHz main.cpp loop rate and compare the emitted count per id against
the real one.

This catches the failure that would otherwise be invisible -- someone edits a
period, the bench still "works", and the synthetic workload quietly stops
resembling the car. It tests the TABLE and the SCHEDULER, not the radio.

    python tools/check_selftest_profile.py
    python tools/check_selftest_profile.py --tolerance 3   # tighter

Exit 0 on pass, 1 on drift, 2 if the capture is not present.

⚠ THE CAPTURE IS NOT IN THE REPO. `.gitignore` excludes `*.csv` because this
repo is public and a vehicle capture is a detailed location history. So this
check cannot run on a fresh clone, which is a deliberate trade and not a bug:
the alternative is publishing where the car has been. Point --capture at a
local copy.
"""

from __future__ import annotations

import argparse
import collections
import csv
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HEADER = os.path.join(ROOT, "firmware", "include", "selftest_profile.h")
CAPTURE = os.path.join(ROOT, "analysis",
                       "captures_2026-09-08_civic_stimulus1.csv")

ROW = re.compile(r"\{\s*0x([0-9A-Fa-f]+),\s*(\d+),\s*(\d+)\s*\}")
STAGGER_MS = 7          # must match selfTestReset() in main.cpp


def load_table(path):
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    # Only the initialiser block, so a hex literal in a comment cannot sneak in.
    start = text.index("SELFTEST_IDS[] = {")
    end = text.index("};", start)
    return [(int(i, 16), int(d), int(p))
            for i, d, p in ROW.findall(text[start:end])]


def load_capture(path):
    with open(path, newline="", encoding="utf-8") as fh:
        rows = list(csv.DictReader(fh))
    ms = [int(r["ms"]) for r in rows]
    counts = collections.Counter(int(r["id"], 16) for r in rows)
    dlcs = {int(r["id"], 16): int(r["dlc"]) for r in rows}
    return max(ms) - min(ms), counts, dlcs, len(rows)


def simulate(table, span_ms):
    nxt = {cid: i * STAGGER_MS for i, (cid, _, _) in enumerate(table)}
    once, count = set(), collections.Counter()
    for now in range(span_ms + 1):
        for cid, _dlc, per in table:
            if per == 0:
                if cid not in once:
                    once.add(cid)
                    count[cid] += 1
                continue
            if now - nxt[cid] < 0:
                continue
            nxt[cid] += per
            count[cid] += 1
    return count


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tolerance", type=float, default=5.0,
                    help="max per-id error, percent (default 5)")
    ap.add_argument("--capture", default=CAPTURE,
                    help="the reference capture (gitignored; see the docstring)")
    args = ap.parse_args(argv)

    if not os.path.exists(args.capture):
        print(f"capture not found: {args.capture}\n"
              "It is gitignored on purpose -- a vehicle capture is a location\n"
              "history and this repo is public. Copy one in locally, or pass\n"
              "--capture <path>. Nothing is wrong with the profile; this check\n"
              "simply has nothing to check it against.")
        return 2

    table = load_table(HEADER)
    span_ms, actual, dlcs, total = load_capture(args.capture)
    sim = simulate(table, span_ms)

    table_ids = {cid for cid, _, _ in table}
    problems = []
    if table_ids != set(actual):
        problems.append(f"id set differs: only in table "
                        f"{sorted(table_ids - set(actual))}, only in capture "
                        f"{sorted(set(actual) - table_ids)}")

    print(f"capture: {total} rows, {len(actual)} ids, {span_ms/1000:.1f}s")
    print(f"{'id':>6} {'dlc':>4} {'sim':>7} {'actual':>7} {'err':>8}  dlc")
    worst = 0.0
    for cid, dlc, _per in table:
        a, s = actual.get(cid, 0), sim[cid]
        err = 100.0 * (s - a) / a if a else float("inf")
        worst = max(worst, abs(err))
        dlc_ok = dlc == dlcs.get(cid)
        if not dlc_ok:
            problems.append(f"0x{cid:03X}: dlc {dlc} but capture has {dlcs.get(cid)}")
        if abs(err) > args.tolerance:
            problems.append(f"0x{cid:03X}: {err:+.1f}% off (tolerance "
                            f"{args.tolerance}%)")
        print(f" 0x{cid:03X} {dlc:>4} {s:>7} {a:>7} {err:>+7.1f}%  "
              f"{'ok' if dlc_ok else 'MISMATCH'}")

    tot_s = sum(sim.values())
    agg = 100.0 * (tot_s - total) / total
    print(f"\naggregate {tot_s} vs {total} ({agg:+.2f}%) = "
          f"{tot_s/(span_ms/1000):.1f} frames/s")
    print(f"worst per-id error {worst:.1f}% (tolerance {args.tolerance}%)")

    if problems:
        print("\nFAIL:")
        for p in problems:
            print(f"  - {p}")
        return 1
    print("\nPASS: the profile still reproduces the capture.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

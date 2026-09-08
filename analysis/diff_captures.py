#!/usr/bin/env python3
"""Find which CAN byte carries a signal, by comparing two captures.

The identification method this project runs on: capture the bus while doing
exactly one deliberate thing, capture it again while doing nothing, and ask
which byte moved in the first and not the second. That is the whole trick --
you never need to know what a byte means to notice that it responds to the
brake pedal.

Two ways to use it:

    # two files
    diff_captures.py baseline.csv brake.csv

    # one file, marking when the action happened (ms, from the ms column)
    diff_captures.py drive.csv --window 20000:26000

The second is usually easier in practice: record one continuous log, note
roughly when you pressed the thing, and let the window be the "stimulus" while
everything outside it is the baseline.

Bytes the firmware already knows are rolling counters or checksums are dropped,
because they move on every frame and would otherwise fill the top of the list.
Pass --keep-heartbeats to see them anyway.
"""

from __future__ import annotations

import argparse
import sys

import canlog
from canlog import ByteStats, Frame


def split_window(frames: list[Frame], lo: int, hi: int) -> tuple[list[Frame], list[Frame]]:
    """Returns (baseline, stimulus): outside the window, and inside it."""
    inside = [f for f in frames if lo <= f.ms <= hi]
    outside = [f for f in frames if f.ms < lo or f.ms > hi]
    return outside, inside


def score(base: ByteStats | None, stim: ByteStats) -> float:
    """How strongly this byte responds to the stimulus and not to the baseline.

    A byte absent from the baseline entirely scores on its stimulus activity
    alone -- that is a genuine finding (an ID that only appears under the
    action), not a missing value to be papered over.
    """
    if stim.static:
        return 0.0

    stim_rate = stim.change_rate
    base_rate = base.change_rate if base is not None else 0.0

    # Reward movement under stimulus, penalise movement at rest. Squared so a
    # byte that is perfectly still at baseline separates sharply from one that
    # merely twitches less.
    margin = stim_rate - base_rate
    if margin <= 0:
        return 0.0

    # Distinct values matter too: a byte flipping between two states is a weaker
    # find than one sweeping a range, but both are real, so this only scales.
    breadth = min(len(stim.values), 32) / 32.0
    return (margin ** 2) * (0.5 + breadth)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("baseline", help="baseline capture, or the single capture when --window is used")
    ap.add_argument("stimulus", nargs="?", help="capture taken while doing the thing")
    ap.add_argument("--window", help="ms range treated as the stimulus, e.g. 20000:26000")
    ap.add_argument("--top", type=int, default=20, help="rows to print (default 20)")
    ap.add_argument("--keep-heartbeats", action="store_true",
                    help="do not drop rolling counters and checksums")
    ap.add_argument("--csv", action="store_true", help="machine-readable output")
    args = ap.parse_args()

    if bool(args.stimulus) == bool(args.window):
        ap.error("give either a second capture or --window, not both and not neither")

    if args.window:
        try:
            lo, hi = (int(x) for x in args.window.split(":"))
        except ValueError:
            ap.error("--window wants two integers like 20000:26000")
        if lo >= hi:
            ap.error("--window start must be before its end")

        frames = canlog.load(args.baseline)
        base_frames, stim_frames = split_window(frames, lo, hi)
        base_label = f"outside {lo}-{hi} ms"
        stim_label = f"inside {lo}-{hi} ms"
    else:
        base_frames = canlog.load(args.baseline)
        stim_frames = canlog.load(args.stimulus)
        base_label, stim_label = args.baseline, args.stimulus

        # Transition counts mean different things in the two file types, so
        # comparing across them would silently rank noise at the top.
        bk, sk = canlog.capture_kind(base_frames), canlog.capture_kind(stim_frames)
        if bk != sk:
            print(f"refusing to compare a {bk} capture against a {sk} one -- "
                  "transition counts are not comparable between them.", file=sys.stderr)
            return 2

    if not stim_frames:
        print("stimulus side is empty; nothing to compare.", file=sys.stderr)
        return 2

    base = canlog.per_byte_stats(base_frames)
    stim = canlog.per_byte_stats(stim_frames)

    rows = []
    for key, s in stim.items():
        b = base.get(key)
        if not args.keep_heartbeats and (s.heartbeat or s.monotonic):
            continue
        sc = score(b, s)
        if sc <= 0:
            continue
        rows.append((sc, key, b, s))

    rows.sort(key=lambda r: r[0], reverse=True)

    if args.csv:
        print("score,id,byte,base_rate,stim_rate,base_values,stim_values,stim_min,stim_max")
        for sc, (cid, i), b, s in rows[:args.top]:
            print(f"{sc:.4f},{cid:03X},{i},"
                  f"{b.change_rate if b else 0:.4f},{s.change_rate:.4f},"
                  f"{len(b.values) if b else 0},{len(s.values)},"
                  f"{min(s.values):02X},{max(s.values):02X}")
        return 0

    print(f"baseline: {base_label}  ({len(base_frames)} frames)")
    print(f"stimulus: {stim_label}  ({len(stim_frames)} frames)")
    print()

    if not rows:
        print("Nothing moved under stimulus that was not already moving at rest.")
        print("Try a wider window, a more forceful action, or --keep-heartbeats")
        print("in case the signal shares a byte with a counter.")
        return 1

    print(f"{'ID':>5} {'byte':>4}  {'base':>6} {'stim':>6}  {'baseline':>12}  {'stimulus':>16}")
    print("-" * 62)
    for sc, (cid, i), b, s in rows[:args.top]:
        base_desc = "absent" if b is None else (
            f"{b.first:02X} static" if b.static else f"{len(b.values)} vals")
        stim_desc = f"{min(s.values):02X}-{max(s.values):02X} ({len(s.values)} vals)"
        print(f"{cid:5X} {i:4d}  "
              f"{(b.change_rate if b else 0):6.2f} {s.change_rate:6.2f}  "
              f"{base_desc:>12}  {stim_desc:>16}")

    print()
    print("base/stim are the fraction of consecutive frames in which the byte moved.")

    # Adjacent active bytes on the same ID are usually one wider value, not two
    # signals -- worth saying so before he goes looking for two separate things.
    hits = {key for _, key, _, _ in rows[:args.top]}
    pairs = sorted((cid, i) for (cid, i) in hits if (cid, i + 1) in hits)
    if pairs:
        print()
        for cid, i in pairs:
            print(f"note: 0x{cid:03X} bytes {i} and {i+1} both respond -- "
                  f"likely one 16-bit value, high byte first.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

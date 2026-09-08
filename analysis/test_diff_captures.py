#!/usr/bin/env python3
"""End-to-end checks for diff_captures.py against synthetic captures.

Synthetic rather than recorded on purpose: here we know the right answer, so a
pass means the tool actually found a planted signal rather than that its output
looked plausible. Real captures are for measuring, not for testing.

The synthetic bus imitates what the Civic actually does -- mostly frozen
payloads with a rolling counter in the last byte -- because that shape is
exactly what would drown a naive diff.
"""

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
TOOL = HERE / "diff_captures.py"

STATIC_IDS = [0x13C, 0x156, 0x158, 0x17C, 0x188, 0x1A6, 0x1D0, 0x1EA]
SIGNAL_ID, SIGNAL_BYTE = 0x1A4, 1
DECOY_ID, DECOY_BYTE = 0x255, 2

# Static at rest, then ticks every frame under stimulus. The heartbeat rule
# classifies it as a counter and drops it, which is right by default and wrong
# here -- it is the case --keep-heartbeats exists for.
FAST_ID, FAST_BYTE = 0x300, 2

HEADER = ["ms", "id", "ext", "dlc", "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"]


def row(ms: int, cid: int, data: list[int]) -> list:
    cells = [f"{v:02X}" for v in data] + [""] * (8 - len(data))
    return [ms, f"{cid:03X}", 0, len(data)] + cells


def write_capture(path: Path, n: int, signal_active: bool) -> None:
    """n frames per ID. Every ID carries a counter in its last byte; the signal
    byte only moves when signal_active."""
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(HEADER)
        ms = 0
        for k in range(n):
            for cid in STATIC_IDS:
                # frozen payload, counter in the last byte
                w.writerow(row(ms, cid, [0x00, 0x11, 0x22, 0x33, k & 0xFF]))
            # the byte under test
            val = (0x40 + (k // 3) % 24) if signal_active else 0x66
            w.writerow(row(ms, SIGNAL_ID, [0x00, val, 0x00, k & 0xFF]))
            # a decoy that moves in BOTH captures and must not outrank the signal
            w.writerow(row(ms, DECOY_ID, [0x00, 0x00, (k * 7) & 0x1F, k & 0xFF]))
            # only starts ticking under stimulus, fast enough to look like a counter
            w.writerow(row(ms, FAST_ID, [0x00, 0x00, (k & 0xFF) if signal_active else 0x00]))
            ms += 10


def run(*args: str) -> tuple[int, str]:
    p = subprocess.run([sys.executable, str(TOOL), *args],
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def parse_csv_out(out: str) -> list[dict]:
    lines = [l for l in out.splitlines() if l.strip()]
    return list(csv.DictReader(lines))


def check(name: str, ok: bool, detail: str = "") -> bool:
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")
    if not ok and detail:
        print(f"         {detail}")
    return ok


def main() -> int:
    failures = 0

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        base = tmp / "baseline.csv"
        stim = tmp / "stimulus.csv"
        write_capture(base, 200, signal_active=False)
        write_capture(stim, 200, signal_active=True)

        # --- two-file mode finds the planted byte, ranked first ---
        rc, out = run(str(base), str(stim), "--csv")
        rows = parse_csv_out(out) if rc == 0 else []
        top = rows[0] if rows else {}
        ok = (rc == 0
              and top.get("id") == f"{SIGNAL_ID:03X}"
              and top.get("byte") == str(SIGNAL_BYTE))
        failures += not check("two-file mode ranks the planted signal first", ok,
                              f"rc={rc} top={top or out[:200]}")

        # --- the counter bytes must not appear at all ---
        ids_bytes = {(r["id"], r["byte"]) for r in rows}
        counters = {(f"{cid:03X}", "4") for cid in STATIC_IDS}
        ok = not (ids_bytes & counters)
        failures += not check("rolling counters are suppressed", ok,
                              f"leaked {sorted(ids_bytes & counters)}")

        # --- a byte moving in BOTH captures must not outrank the signal ---
        decoy_rank = next((i for i, r in enumerate(rows)
                           if r["id"] == f"{DECOY_ID:03X}"
                           and r["byte"] == str(DECOY_BYTE)), None)
        ok = decoy_rank is None or decoy_rank > 0
        failures += not check("a byte active in both captures does not win", ok,
                              f"decoy ranked {decoy_rank}")

        # --- a byte that only starts ticking under stimulus is hidden by
        #     default (it looks like a counter) and --keep-heartbeats reveals it ---
        def has_fast(text: str) -> bool:
            return any(r["id"] == f"{FAST_ID:03X}" and r["byte"] == str(FAST_BYTE)
                       for r in parse_csv_out(text))

        rc, out = run(str(base), str(stim), "--csv")
        ok = rc == 0 and not has_fast(out)
        failures += not check("a counter-looking byte is hidden by default", ok)

        rc, out = run(str(base), str(stim), "--csv", "--keep-heartbeats")
        ok = rc == 0 and has_fast(out)
        failures += not check("--keep-heartbeats reveals it", ok, out[:200])

        # --- window mode on a single file ---
        single = tmp / "single.csv"
        with open(single, "w", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            w.writerow(HEADER)
            ms = 0
            for k in range(300):
                active = 100 <= k < 160        # ms 1000..1600
                val = (0x40 + (k // 3) % 24) if active else 0x66
                w.writerow(row(ms, SIGNAL_ID, [0x00, val, 0x00, k & 0xFF]))
                for cid in STATIC_IDS:
                    w.writerow(row(ms, cid, [0x00, 0x11, 0x22, 0x33, k & 0xFF]))
                ms += 10

        rc, out = run(str(single), "--window", "1000:1590", "--csv")
        rows = parse_csv_out(out) if rc == 0 else []
        top = rows[0] if rows else {}
        ok = (rc == 0
              and top.get("id") == f"{SIGNAL_ID:03X}"
              and top.get("byte") == str(SIGNAL_BYTE))
        failures += not check("window mode finds the signal inside the window", ok,
                              f"rc={rc} top={top or out[:200]}")

        # --- a window with no signal reports nothing rather than inventing one ---
        rc, out = run(str(single), "--window", "2000:2500", "--csv")
        rows = parse_csv_out(out) if rc == 0 else []
        ok = rc != 0 or not rows
        failures += not check("a quiet window yields no findings", ok,
                              f"rc={rc} rows={len(rows)}")

        # --- refuses to compare mismatched capture kinds ---
        chg = tmp / "changes.csv"
        with open(chg, "w", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            w.writerow(["ms", "id", "ext", "dlc", "changed",
                        "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"])
            for k in range(40):
                w.writerow([k * 10, f"{SIGNAL_ID:03X}", 0, 4, "02",
                            "00", f"{k:02X}", "00", "00", "", "", "", ""])
        rc, out = run(str(base), str(chg))
        ok = rc == 2 and "refusing" in out
        failures += not check("refuses raw-vs-changes comparison", ok, out[:160])

        # --- both files given AND --window is a usage error ---
        rc, out = run(str(base), str(stim), "--window", "0:100")
        failures += not check("rejects both a second file and --window", rc != 0)

    print()
    print("FAILURES:", failures)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())

"""AP <-> STA soak test: toggle the bench hub fifty times and count what happens.

WHY THIS EXISTS
---------------
Two things about the Wi-Fi path were being argued about from a single
observation each:

  * `wifi_init_default: netstack cb reg failed with 12308` appeared once on an
    AP<->STA transition and RECOVERED. A fault that recovers leaves no trace,
    so a one-off sighting cannot tell you whether it happens on 2% of
    transitions or 90%, whether it ever fails to recover, or whether it leaks.
  * STA-loss detection was moved from a poll to a disconnect event. The claim
    "seconds, not 2.5 minutes" is worth exactly as much as the distribution
    behind it.

Neither is answerable by staring at the code. This runs the transition fifty
times with both clocks in one process, so every number below is measured:

    detection latency   hotspot stop returns -> "[hublink] STA lost"
    fallback latency    that line -> the AP is serving again
    rejoin latency      hotspot start returns -> "[hublink] STA up"
    12308 rate          occurrences / transitions, and did it recover
    reboots             boot banner seen mid-run = the board reset
    heap                free heap per transition, first vs last, slope

WHAT IT CANNOT TELL YOU
-----------------------
This toggles a Windows Mobile Hotspot, which vanishes cleanly. A car losing
the hub is usually a RANGE problem: the AP fades rather than disappearing, and
the driver may cling to a marginal link far longer than it takes to notice a
clean disappearance. So this measures the FLOOR of detection latency, not the
worst case. Treat the numbers as "no worse than the driving case cannot be
inferred from this" -- walking the board out of range is a different test.

USAGE
-----
    # board on USB, hotspot configured, close the PlatformIO monitor first
    python tools/soak_wifi.py --port COM7 --cycles 50

    python tools/soak_wifi.py --port COM7 --cycles 3 --dwell 15   # smoke test

Writes a per-cycle CSV next to the report so the distribution survives the
summary.
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import statistics
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field

try:
    import serial  # pyserial
except ImportError:
    print("needs pyserial:  pip install pyserial", file=sys.stderr)
    raise

HERE = os.path.dirname(os.path.abspath(__file__))
HOTSPOT_PS1 = os.path.join(HERE, "hotspot.ps1")

# Serial patterns. Kept as literals rather than shared constants with the
# firmware on purpose: if someone changes a log line, this test should fail
# loudly rather than quietly stop matching.
RE_STA_LOST = re.compile(r"\[hublink\] STA lost \((\w+), reason=(\d+)\)")
RE_STA_UP = re.compile(r"\[hublink\] STA up: ip=(\S+)")
RE_FALLBACK = re.compile(r"\[hublink\] fallback .*fallback=(\d+)ms")
RE_HEAP = re.compile(r"heap=(\d+) minheap=(\d+)")
RE_NETSTACK = re.compile(r"netstack cb reg failed with (\d+)")
# The ROM prints these on every reset. Seeing one MID-RUN means the board
# restarted, which is the failure mode a recovering fault would otherwise hide.
RE_BOOT = re.compile(r"(rst:0x[0-9a-fA-F]+|ESP-ROM:esp32)")
RE_PANIC = re.compile(r"(Guru Meditation|abort\(\) was called|StoreProhibited|"
                      r"LoadProhibited|assert failed)")


@dataclass
class Line:
    t: float
    text: str


@dataclass
class Cycle:
    n: int
    t_off: float = 0.0
    t_lost: float | None = None
    t_on: float = 0.0
    t_up: float | None = None
    detect_ms: float | None = None
    rejoin_ms: float | None = None
    fallback_ms: int | None = None
    drop_path: str = ""       # "event" or "poll"
    reason: int | None = None
    heap: int | None = None
    netstack: int = 0
    reboot: bool = False


@dataclass
class Totals:
    netstack: int = 0
    reboots: int = 0
    panics: int = 0
    unrecovered: int = 0
    lines: list = field(default_factory=list)


class SerialTap:
    """Reads the port in a thread and timestamps every line on ONE clock.

    The toggling and the serial reading have to share a clock or the latency
    numbers are meaningless -- comparing a PowerShell timestamp to a
    PlatformIO monitor timestamp would measure the difference between two
    clocks as well as the thing being tested.
    """

    def __init__(self, port: str, baud: int = 115200):
        self.ser = serial.Serial(port, baud, timeout=0.2)
        self.lines: list[Line] = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._buf = b""
        self._th = threading.Thread(target=self._run, daemon=True)
        self._th.start()

    def _run(self):
        while not self._stop.is_set():
            try:
                chunk = self.ser.read(4096)
            except Exception as exc:              # port yanked mid-run
                with self._lock:
                    self.lines.append(Line(time.monotonic(),
                                           f"<<serial error: {exc}>>"))
                return
            if not chunk:
                continue
            self._buf += chunk
            while b"\n" in self._buf:
                raw, self._buf = self._buf.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").rstrip("\r")
                with self._lock:
                    self.lines.append(Line(time.monotonic(), text))

    def snapshot(self) -> list[Line]:
        with self._lock:
            return list(self.lines)

    def wait_for(self, pattern: re.Pattern, since_idx: int,
                 timeout: float) -> tuple[Line, re.Match, int] | None:
        """Block until a line after `since_idx` matches. Returns (line, m, idx)."""
        deadline = time.monotonic() + timeout
        i = since_idx
        while time.monotonic() < deadline:
            snap = self.snapshot()
            while i < len(snap):
                m = pattern.search(snap[i].text)
                if m:
                    return snap[i], m, i
                i += 1
            time.sleep(0.02)
        return None

    def count(self) -> int:
        with self._lock:
            return len(self.lines)

    def close(self):
        self._stop.set()
        self._th.join(timeout=2)
        try:
            self.ser.close()
        except Exception:
            pass


def hotspot(action: str, timeout: float = 90.0) -> str:
    out = subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
         "-File", HOTSPOT_PS1, "-Action", action],
        capture_output=True, text=True, timeout=timeout)
    text = (out.stdout or "").strip().splitlines()
    return text[-1].strip() if text else f"<no output, rc={out.returncode}>"


def scan_window(tap: SerialTap, lo: int, hi: int, cyc: Cycle, tot: Totals):
    """Attribute per-line findings in [lo, hi) to this cycle."""
    for ln in tap.snapshot()[lo:hi]:
        if RE_NETSTACK.search(ln.text):
            cyc.netstack += 1
            tot.netstack += 1
        if RE_PANIC.search(ln.text):
            tot.panics += 1
        m = RE_HEAP.search(ln.text)
        if m:
            cyc.heap = int(m.group(1))
        m = RE_FALLBACK.search(ln.text)
        if m:
            cyc.fallback_ms = int(m.group(1))


def run(args) -> int:
    state = hotspot("state")
    if state == "NoProfile":
        print("ERROR: no active internet connection profile, so Windows will "
              "not expose the tethering manager. Connect to a network first.")
        return 2
    print(f"hotspot state at start: {state}")

    tap = SerialTap(args.port, args.baud)
    time.sleep(1.0)
    if tap.count() == 0:
        print("WARNING: nothing on the serial port yet. If the PlatformIO "
              "monitor is open, close it -- the port is exclusive.")

    # Start from a known state: hotspot ON and the board joined.
    if state != "On":
        print(f"  start -> {hotspot('start')}")
    print("  waiting for the board to join ...")
    got = tap.wait_for(RE_STA_UP, 0, timeout=args.join_timeout)
    if not got:
        print(f"ERROR: board never reported 'STA up' within "
              f"{args.join_timeout:.0f}s. Check the SSID/band (the S3 is "
              f"2.4 GHz only) and that the firmware is the rotated build.")
        tap.close()
        return 3
    print(f"  joined: {got[0].text}")

    cycles: list[Cycle] = []
    tot = Totals()

    for n in range(1, args.cycles + 1):
        cyc = Cycle(n=n)
        idx0 = tap.count()

        # --- take the hub away ------------------------------------------
        r = hotspot("stop")
        cyc.t_off = time.monotonic()
        got = tap.wait_for(RE_STA_LOST, idx0, timeout=args.drop_timeout)
        if got:
            ln, m, idx = got
            cyc.t_lost = ln.t
            cyc.detect_ms = (ln.t - cyc.t_off) * 1000.0
            cyc.drop_path = m.group(1)
            cyc.reason = int(m.group(2))
        else:
            tot.unrecovered += 1
            print(f"  [{n:>3}] NO DROP DETECTED within "
                  f"{args.drop_timeout:.0f}s (stop -> {r})")

        time.sleep(args.dwell)
        scan_window(tap, idx0, tap.count(), cyc, tot)

        # --- give it back -----------------------------------------------
        idx1 = tap.count()
        r = hotspot("start")
        cyc.t_on = time.monotonic()
        got = tap.wait_for(RE_STA_UP, idx1, timeout=args.join_timeout)
        if got:
            cyc.t_up = got[0].t
            cyc.rejoin_ms = (got[0].t - cyc.t_on) * 1000.0
        else:
            print(f"  [{n:>3}] NO REJOIN within {args.join_timeout:.0f}s "
                  f"(start -> {r})")

        time.sleep(args.dwell)
        end = tap.count()
        scan_window(tap, idx1, end, cyc, tot)

        for ln in tap.snapshot()[idx0:end]:
            if RE_BOOT.search(ln.text):
                cyc.reboot = True
                tot.reboots += 1

        cycles.append(cyc)
        d = f"{cyc.detect_ms/1000:6.2f}s" if cyc.detect_ms is not None else "  MISS "
        j = f"{cyc.rejoin_ms/1000:6.2f}s" if cyc.rejoin_ms is not None else "  MISS "
        print(f"  [{n:>3}/{args.cycles}] detect={d} rejoin={j} "
              f"path={cyc.drop_path or '-':5} reason={cyc.reason if cyc.reason is not None else '-':>3} "
              f"heap={cyc.heap or 0:>6} 12308={cyc.netstack}"
              + ("  REBOOT" if cyc.reboot else ""))

    tap.close()
    report(cycles, tot, args)
    return 0


def stat_block(name: str, vals: list[float], unit: str = "s") -> str:
    if not vals:
        return f"{name:<18} no samples"
    scale = 1000.0 if unit == "s" else 1.0
    v = sorted(x / scale for x in vals)
    med = statistics.median(v)
    p95 = v[max(0, int(round(0.95 * (len(v) - 1))))]
    return (f"{name:<18} n={len(v):<4} min={v[0]:6.2f}{unit} "
            f"med={med:6.2f}{unit} p95={p95:6.2f}{unit} max={v[-1]:6.2f}{unit}")


def report(cycles: list[Cycle], tot: Totals, args):
    detects = [c.detect_ms for c in cycles if c.detect_ms is not None]
    rejoins = [c.rejoin_ms for c in cycles if c.rejoin_ms is not None]
    fallbacks = [float(c.fallback_ms) for c in cycles if c.fallback_ms is not None]
    heaps = [(c.n, c.heap) for c in cycles if c.heap]

    print()
    print("=" * 72)
    print(f"SOAK RESULT  --  {len(cycles)} cycles, dwell {args.dwell}s")
    print("=" * 72)
    print(stat_block("detect (drop)", detects))
    print(stat_block("rejoin", rejoins))
    print(stat_block("fallback->AP", fallbacks, unit="ms"))
    print()
    print("READ 'detect' AS THE RESULT. 'rejoin' is dominated by the firmware's")
    print("own 60s retry timer, not by anything the radio did: the board is in")
    print("AP mode and only looks for the hub on that cadence, so a rejoin of")
    print("tens of seconds is the timer working, not a fault. The number that")
    print("matters is how long the board spends with NO interface, which is")
    print("detect + fallback.")
    print()
    print(f"missed drops        {sum(1 for c in cycles if c.detect_ms is None)}"
          f" / {len(cycles)}")
    print(f"missed rejoins      {sum(1 for c in cycles if c.rejoin_ms is None)}"
          f" / {len(cycles)}")
    ev = sum(1 for c in cycles if c.drop_path == "event")
    po = sum(1 for c in cycles if c.drop_path == "poll")
    print(f"drop path           event={ev}  poll={po}"
          "   (poll > 0 means the event handler missed one)")
    reasons = {}
    for c in cycles:
        if c.reason is not None:
            reasons[c.reason] = reasons.get(c.reason, 0) + 1
    print(f"disconnect reasons  {reasons or '-'}")
    print()
    print(f"12308 occurrences   {tot.netstack}"
          f"   ({100.0*tot.netstack/max(1,len(cycles)):.0f}% of cycles)")
    print(f"reboots             {tot.reboots}")
    print(f"panics/asserts      {tot.panics}")
    print(f"unrecovered         {tot.unrecovered}"
          "   (cycles where the board never noticed the hub had gone)")
    print()
    if len(heaps) >= 2:
        first, last = heaps[0][1], heaps[-1][1]
        drift = last - first
        per = drift / max(1, heaps[-1][0] - heaps[0][0])
        print(f"heap first          {first} B  (cycle {heaps[0][0]})")
        print(f"heap last           {last} B  (cycle {heaps[-1][0]})")
        print(f"drift               {drift:+d} B total, {per:+.1f} B/cycle")
        print("                    A steady negative slope is a leak. Noise of "
              "a few hundred bytes is not.")
    else:
        print("heap                not enough samples")

    out = args.csv or os.path.join(HERE, "soak_wifi_result.csv")
    with open(out, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["cycle", "detect_ms", "rejoin_ms", "fallback_ms",
                    "drop_path", "reason", "heap", "netstack_12308", "reboot"])
        for c in cycles:
            w.writerow([c.n,
                        f"{c.detect_ms:.1f}" if c.detect_ms is not None else "",
                        f"{c.rejoin_ms:.1f}" if c.rejoin_ms is not None else "",
                        c.fallback_ms if c.fallback_ms is not None else "",
                        c.drop_path, c.reason if c.reason is not None else "",
                        c.heap or "", c.netstack, int(c.reboot)])
    print()
    print(f"per-cycle CSV -> {out}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="e.g. COM7")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--cycles", type=int, default=50)
    ap.add_argument("--dwell", type=float, default=20.0,
                    help="seconds to sit in each state. Must exceed the "
                         "firmware's quick retry (10s) so a rejoin is "
                         "attributable to the hotspot coming back rather than "
                         "to the retry timer.")
    ap.add_argument("--drop-timeout", type=float, default=180.0,
                    help="how long to wait for a drop before calling it missed."
                         " Generous on purpose: the OLD firmware took ~150s.")
    ap.add_argument("--join-timeout", type=float, default=120.0)
    ap.add_argument("--csv")
    args = ap.parse_args(argv)

    if args.dwell < 12:
        print("WARNING: dwell below the 10s quick retry; rejoin timings will "
              "be confounded by the retry timer.")
    try:
        return run(args)
    except KeyboardInterrupt:
        print("\ninterrupted; leaving the hotspot as-is")
        return 130


if __name__ == "__main__":
    raise SystemExit(main())

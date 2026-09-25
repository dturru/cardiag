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
from datetime import datetime, timezone

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
RE_HEAP = re.compile(r"heap=(\d+) minheap=(\d+)(?: largest=(\d+))?")
# ❌ CORRECTED 2026-09-25. This comment used to say min_free "only ever FALLS,
# so it is the leak signal that survives a reboot". It does not survive one:
# minheap= is esp_get_minimum_free_heap_size(), a PER-BOOT low-water mark that
# restarts at every reset. Within a boot it is headroom (how close the board
# came); across boots it compares two different runs. The leak signal is the
# trend of heap= and largest= INSIDE a boot segment -- soak_summary.py.
RE_NETSTACK = re.compile(r"netstack cb reg failed with (\d+)")
# The ROM prints these on every reset. Seeing one MID-RUN means the board
# restarted, which is the failure mode a recovering fault would otherwise hide.
RE_BOOT = re.compile(r"(rst:0x[0-9a-fA-F]+|ESP-ROM:esp32)")
# The firmware's own classification, printed once per boot in setup().
# Strictly better than decoding rst:0x.. by hand, and it is the line that
# separates "the bench supply sagged" from "our code hung".
RE_RESET_REASON = re.compile(r"\[boot\] RESET REASON:\s*(\w+)")
# Max loop() pass since boot, appended to the hublink stats line. Resets with
# the board. The non-blocking join is supposed to keep it well under a second.
RE_LOOPMAX = re.compile(r"loopmax=(\d+)us")
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
    minheap: int | None = None
    largest: int | None = None
    netstack: int = 0
    reboot: bool = False
    # ⭐ WHY the board rebooted, from the firmware's own [boot] RESET REASON
    # line. A soak that counts reboots but cannot say why is a reboot counter,
    # not a diagnosis. BROWNOUT on the bench means the supply sagged -- the
    # harness or the USB cable -- and is a DIFFERENT finding from TASK_WDT or
    # PANIC, which are ours. Empty when no reset was observed this cycle.
    reset_reason: str = ""
    # Max loop() pass since boot, microseconds, last value seen this cycle.
    # Firmware older than the non-blocking join does not print it: None.
    loop_max_us: int | None = None


CSV_FIELDS = ["cycle", "detect_ms", "rejoin_ms", "fallback_ms", "drop_path",
              "reason", "heap", "minheap", "largest_block", "netstack_12308",
              "reboot", "reset_reason", "loop_max_us"]


def csv_path(args) -> str:
    return args.csv or os.path.join(HERE, "soak_wifi_result.csv")


def raw_log_path(args) -> str:
    """Next to the CSV unless given: <csv stem>.serial.log."""
    if getattr(args, "raw_log", None):
        return args.raw_log
    return os.path.splitext(csv_path(args))[0] + ".serial.log"


def write_cycle_row(args, c) -> None:
    """Append ONE cycle and flush.

    🛑 The uninterruptible-work rule: an overnight run can be cut at any time,
    and a killed run must leave usable partial data. This used to be a single
    write at the end of report(), so a soak killed at cycle 199 of 200 left
    nothing at all.
    """
    out = csv_path(args)
    new = not os.path.exists(out) or os.path.getsize(out) == 0
    with open(out, "a", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        if new:
            w.writerow(CSV_FIELDS)
        w.writerow([c.n,
                    f"{c.detect_ms:.1f}" if c.detect_ms is not None else "",
                    f"{c.rejoin_ms:.1f}" if c.rejoin_ms is not None else "",
                    c.fallback_ms if c.fallback_ms is not None else "",
                    c.drop_path, c.reason if c.reason is not None else "",
                    c.heap or "", c.minheap or "",
                    c.largest or "", c.netstack, int(c.reboot),
                    c.reset_reason,
                    c.loop_max_us if c.loop_max_us is not None else ""])
        fh.flush()
        os.fsync(fh.fileno())


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

    def __init__(self, port: str, baud: int = 115200,
                 raw_path: str | None = None):
        # 🐛 DTR/RTS PINNED BEFORE OPEN, exactly as tools/serial_capture.py does.
        # This used to be serial.Serial(port, baud, ...), which opens with
        # pyserial's defaults: DTR asserted on open, both dropped on close. On
        # the S3's native USB-Serial-JTAG those lines are EN/BOOT, so a soak
        # tool at defaults can reset the board it is counting resets of
        # (CLAUDE.md, hardware guardrails). ⚪ Whether it DID in any past run is
        # untested -- the guardrail table lists raw pyserial at defaults as
        # "assume it resets".
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.2
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.dsrdtr = False
        self.ser.open()
        # ⭐ RAW SERIAL, PER LINE, ON DISK AS IT ARRIVES. The in-memory list
        # dies with the process; this file does not. Each line carries the
        # host wall clock (to line up with the hotspot calls and other logs)
        # and the monotonic clock every latency here is measured on.
        self._raw = open(raw_path, "a", encoding="utf-8", newline="") \
            if raw_path else None
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
                self._record(f"<<serial error: {exc}>>")
                return
            if not chunk:
                continue
            self._buf += chunk
            while b"\n" in self._buf:
                raw, self._buf = self._buf.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").rstrip("\r")
                self._record(text)

    def _record(self, text: str) -> None:
        """One line: into memory for the matchers, and onto disk for good.

        Flushed AND fsynced per line. A reset, a panic or a killed run then
        costs at most the line being received, never the lines before it --
        the lines right before a crash are the ones that matter.
        """
        t = time.monotonic()
        with self._lock:
            self.lines.append(Line(t, text))
        if self._raw is not None:
            wall = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
            self._raw.write(f"{wall}\t{t:.3f}\t{text}\n")
            self._raw.flush()
            os.fsync(self._raw.fileno())

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
        # A line still waiting for its newline is kept, and marked as such.
        if self._buf:
            self._record(self._buf.decode("utf-8", "replace").rstrip("\r")
                         + "  <<partial: no newline before close>>")
            self._buf = b""
        try:
            self.ser.close()
        except Exception:
            pass
        if self._raw is not None:
            self._raw.close()
            self._raw = None


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
            cyc.minheap = int(m.group(2))
            if m.group(3):
                cyc.largest = int(m.group(3))
        m = RE_FALLBACK.search(ln.text)
        if m:
            cyc.fallback_ms = int(m.group(1))
        m = RE_LOOPMAX.search(ln.text)
        if m:
            cyc.loop_max_us = int(m.group(1))


def run(args) -> int:
    state = hotspot("state")
    if state == "NoProfile":
        print("ERROR: no active internet connection profile, so Windows will "
              "not expose the tethering manager. Connect to a network first.")
        return 2
    print(f"hotspot state at start: {state}")

    tap = SerialTap(args.port, args.baud, raw_path=raw_log_path(args))
    print(f"raw serial -> {raw_log_path(args)}  (every line, host-timestamped, "
          f"fsynced as it arrives)")
    time.sleep(1.0)
    if tap.count() == 0:
        print("WARNING: nothing on the serial port yet. If the PlatformIO "
              "monitor is open, close it -- the port is exclusive.")

    # Start from a known state: hotspot ON and the board OBSERVED to join.
    #
    # 🐛 A BOARD THAT IS ALREADY JOINED NEVER SAYS "STA up" AGAIN.
    # This used to wait for that line unconditionally, which deadlocked for the
    # whole join timeout in the ORDINARY case -- hotspot already on, board
    # joined at boot -- and then blamed the SSID, the band and the firmware.
    # Three wrong diagnoses for a tool that was waiting on an event which had
    # already happened before it started looking.
    #
    # The fix is not to guess the state: it is to CREATE the transition we need
    # to observe. One extra toggle before cycle 1 costs ~20 s and makes the
    # starting point a measurement instead of an assumption.
    if state == "On":
        print("  hotspot already on; cycling it once to observe a real join")
        print(f"  stop  -> {hotspot('stop')}")
        # Not waited on: if the board was not joined there is nothing to lose,
        # and that is fine. The dwell is what guarantees it has settled.
        tap.wait_for(RE_STA_LOST, 0, timeout=args.drop_timeout)
        time.sleep(2.0)

    mark = len(tap.snapshot())
    print(f"  start -> {hotspot('start')}")
    print("  waiting for the board to join ...")
    got = tap.wait_for(RE_STA_UP, mark, timeout=args.join_timeout)
    if not got:
        print(f"ERROR: board never reported 'STA up' within "
              f"{args.join_timeout:.0f}s of the hotspot coming up. Check the "
              f"SSID/band (the S3 is 2.4 GHz only) and that the firmware is "
              f"the rotated build.")
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
            m = RE_RESET_REASON.search(ln.text)
            if m:
                cyc.reset_reason = m.group(1)
                # A reset reason line IS a boot, even if the ROM banner was
                # missed -- USB CDC re-enumeration can swallow the first lines.
                if not cyc.reboot:
                    cyc.reboot = True
                    tot.reboots += 1

        cycles.append(cyc)
        # 🛑 UNINTERRUPTIBLE-WORK RULE: flush this cycle to disk NOW. A
        # 200-cycle overnight soak that is killed at cycle 180 must leave 180
        # usable rows, not an empty file. The CSV used to be written once in
        # report(), so any interruption lost the entire run.
        write_cycle_row(args, cyc)
        d = f"{cyc.detect_ms/1000:6.2f}s" if cyc.detect_ms is not None else "  MISS "
        j = f"{cyc.rejoin_ms/1000:6.2f}s" if cyc.rejoin_ms is not None else "  MISS "
        print(f"  [{n:>3}/{args.cycles}] detect={d} rejoin={j} "
              f"path={cyc.drop_path or '-':5} reason={cyc.reason if cyc.reason is not None else '-':>3} "
              f"heap={cyc.heap or 0:>6} 12308={cyc.netstack}"
              + ("  REBOOT" if cyc.reboot else ""))

    tap.close()
    rc = report(cycles, tot, args)
    return rc


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
        print(f"heap first          {first} B  (cycle {heaps[0][0]})")
        print(f"heap last           {last} B  (cycle {heaps[-1][0]})")

        # 🐛 (last - first) / cycles IS WRONG ACROSS A REBOOT, and it
        # understated a real leak by 3x. A reboot restores the heap, so a run
        # that leaked 6.3 kB/cycle until it exhausted itself at cycle 34 and
        # then leaked another 94 kB reported as "-1,922 B/cycle" -- a number
        # small enough to read as drift. The per-cycle DELTAS are what the
        # leak actually is; the reboot is a discontinuity, not a data point.
        deltas = [(c, h - prev_h)
                  for (prev_c, prev_h), (c, h) in zip(heaps, heaps[1:])
                  if h <= prev_h or (h - prev_h) < 50_000]
        if deltas:
            vals = sorted(d for _, d in deltas)
            med = vals[len(vals) // 2]
            neg = sum(1 for d in vals if d < 0)
            print(f"per-cycle delta     median {med:+.0f} B   "
                  f"negative on {neg}/{len(vals)} cycles")
            if neg == len(vals) and med < -256:
                print(f"                    ** LEAK **: every single cycle "
                      f"lost heap. At {abs(med):.0f} B/cycle this exhausts "
                      f"{first} B in about {first // max(1, abs(int(med)))} "
                      f"cycles.")
            elif neg > len(vals) * 0.8 and med < -256:
                print("                    ** probable leak **: most cycles "
                      "lost heap.")
            else:
                print("                    No consistent slope -- this is "
                      "noise, not a leak.")
        jumps = [c for (pc, ph), (c, h) in zip(heaps, heaps[1:])
                 if h - ph >= 50_000]
        if jumps:
            print(f"                    heap JUMPED UP at cycle(s) "
                  f"{', '.join(str(c) for c in jumps)} -- that is a reboot, "
                  f"and the slope is measured within segments, not across it.")
    else:
        print("heap                not enough samples")

    # --- explicit verdict -------------------------------------------------
    #
    # A run that only prints numbers gets read optimistically. These are the
    # agreed pass criteria for "the leak is fixed", checked rather than eyeballed.
    # --- loop latency ---------------------------------------------------
    # Since-boot max, so the last cycle's value covers the whole run (per
    # boot). One second is far past the ~100 ms-per-call design and past any
    # plausible single HTTP request; crossing it means something in loop()
    # still blocks the way the old 8 s join did.
    loops = [c.loop_max_us for c in cycles if c.loop_max_us is not None]
    mins = [(c.n, c.minheap) for c in cycles if c.minheap]
    larges = [(c.n, c.largest) for c in cycles if c.largest]
    fails: list[str] = []
    if loops:
        worst = max(loops)
        print(f"loop max            {worst / 1000:.1f} ms (since boot, worst "
              f"cycle)")
        if worst > 1_000_000:
            fails.append(f"a loop() pass took {worst / 1000:.0f} ms -- "
                         f"something still blocks")
    else:
        print("loop max            not reported -- firmware predates "
              "`loopmax=` in the stats line")

    if len(heaps) >= 2:
        d = [h - ph for (_, ph), (_, h) in zip(heaps, heaps[1:])
             if h - ph < 50_000]
        if d:
            med = sorted(d)[len(d) // 2]
            if med < -256:
                fails.append(f"median per-cycle heap delta {med:+.0f} B "
                             f"(want approximately 0)")
    if len(mins) >= 2:
        # HEADROOM, not a verdict. minheap= is a per-boot low-water mark and
        # restarts at every reset, so a "fall across the run" can be two
        # different boots. The leak verdict is per boot segment, in
        # soak_summary.py.
        lo = min(v for _, v in mins)
        print(f"min free heap       lowest {lo} B (headroom; per-boot, "
              f"restarts at each reset -- not a leak signal)")
    else:
        print("min free heap       not reported -- firmware predates "
              "`minheap=` in the stats line")

    if len(larges) >= 2:
        lo = min(v for _, v in larges)
        drop = larges[0][1] - larges[-1][1]
        print(f"largest free block  {larges[0][1]} -> {larges[-1][1]} B "
              f"(min seen {lo})")
        # Fragmentation kills with free heap to spare: a flat total and a
        # falling largest block still ends in a failed allocation.
        if drop > 8192:
            fails.append(f"largest free block fell {drop} B -- fragmentation, "
                         f"even if total free heap looks flat")
    else:
        print("largest free block  not reported -- firmware predates "
              "`largest=` in the stats line")

    if tot.reboots:
        fails.append(f"{tot.reboots} reboot(s)")

    # ⭐ CLASSIFY EVERY RESET. "3 reboots" is not a finding; "3 BROWNOUTs" and
    # "3 TASK_WDTs" are completely different findings, and on the bench only
    # one of them is about our code. The firmware prints its own reason at
    # boot, so this is read, not inferred.
    reasons: dict[str, list[int]] = {}
    for c in cycles:
        if c.reboot:
            reasons.setdefault(c.reset_reason or "UNREPORTED", []).append(c.n)
    if reasons:
        print()
        print("reset reasons")
        for name, at in sorted(reasons.items()):
            where = ", ".join(str(n) for n in at[:12])
            more = f" (+{len(at) - 12} more)" if len(at) > 12 else ""
            print(f"  {name:<11} {len(at):>3}  at cycle(s) {where}{more}")
        supply = sum(len(v) for k, v in reasons.items() if k == "BROWNOUT")
        ours = sum(len(v) for k, v in reasons.items()
                   if k in ("PANIC", "TASK_WDT", "INT_WDT", "WDT"))
        if supply:
            print(f"  -> {supply} BROWNOUT: the SUPPLY sagged, not the "
                  f"firmware. On the bench that is the harness or the USB "
                  f"cable. Re-run on USB power from a direct port before "
                  f"reading anything else into it.")
        if ours:
            print(f"  -> {ours} PANIC/WDT: these ARE ours and are the real "
                  f"finding of this run.")
        if "UNREPORTED" in reasons:
            print(f"  -> UNREPORTED means the board booted without printing "
                  f"[boot] RESET REASON -- firmware older than that line. "
                  f"Reflash before trusting the classification.")
    if tot.panics:
        fails.append(f"{tot.panics} panic/assert(s)")

    print()
    if fails:
        print("VERDICT: ** FAIL **")
        for f in fails:
            print(f"  - {f}")
    else:
        print(f"VERDICT: PASS over {len(cycles)} cycles -- heap flat, no "
              f"fragmentation trend, no reboots or panics.")

    out = csv_path(args)
    print()
    print(f"per-cycle CSV -> {out}   (written and flushed EVERY cycle)")
    # Non-zero on FAIL so this is usable from a script, not just by eye.
    return 1 if fails else 0


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
    ap.add_argument("--raw-log", dest="raw_log",
                    help="raw serial, one line per board line, host-"
                         "timestamped and fsynced (default: <csv stem>"
                         ".serial.log next to the CSV)")
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

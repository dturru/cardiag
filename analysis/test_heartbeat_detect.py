#!/usr/bin/env python3
"""Check the sniffer's heartbeat-byte heuristic against real Civic frames.

This pins canlog.ByteStats.heartbeat, which is the Python mirror of
isHeartbeatByte() in firmware/src/sniffer.cpp. Testing the shared module rather
than a private copy means the diff tool and this test cannot disagree about what
a heartbeat is. The C remains a separate implementation -- change one, change
the other.

Why it matters: the 2026-09-07 capture showed most Honda IDs carry a rolling
counter plus a checksum in their last byte. Without suppressing those, every
row in the sniffer table shows as "changed" and the tool is useless. The
heuristic is that a byte differing from the previous frame on nearly every
frame is a heartbeat, not a signal.

Payloads below are verbatim from the 2026-09-07 capture.
"""

import canlog
from canlog import SNIFF_MIN_SAMPLES


def heartbeat_bytes(frames):
    """Replays `frames` (a list of equal-length byte lists) through the shared
    stats code and returns the byte indices judged heartbeats."""
    recs = [canlog.Frame(ms=i * 10, id=0x100, ext=False, dlc=len(f), data=list(f))
            for i, f in enumerate(frames)]
    stats = canlog.per_byte_stats(recs)
    return {i for (_id, i), st in stats.items() if st.heartbeat}


def cycle(seq, n):
    """Repeat an observed payload cycle out to n frames."""
    return [list(seq[i % len(seq)]) for i in range(n)]


# --- observed cycles, straight from the capture -----------------------------

CASES = [
    (
        "0x188  counter+checksum in the last byte",
        cycle([
            [0x00, 0x00, 0x00, 0x01, 0x00, 0x33],
            [0x00, 0x00, 0x00, 0x01, 0x00, 0x06],
            [0x00, 0x00, 0x00, 0x01, 0x00, 0x15],
            [0x00, 0x00, 0x00, 0x01, 0x00, 0x24],
        ], 40),
        {5},
    ),
    (
        "0x1ED  counter+checksum in the last byte",
        cycle([
            [0x01, 0xFF, 0x0D],
            [0x01, 0xFF, 0x1C],
            [0x01, 0xFF, 0x2B],
            [0x01, 0xFF, 0x3A],
        ], 40),
        {2},
    ),
    (
        "0x13C  counter+checksum in the last byte",
        cycle([
            [0xFF, 0xC7, 0x01, 0x62, 0x00, 0x40, 0x04, 0x15],
            [0xFF, 0xC7, 0x01, 0x62, 0x00, 0x40, 0x04, 0x24],
            [0xFF, 0xC7, 0x01, 0x62, 0x00, 0x40, 0x04, 0x33],
            [0xFF, 0xC7, 0x01, 0x62, 0x00, 0x40, 0x04, 0x06],
        ], 40),
        {7},
    ),
    (
        "0x255  counter AND complement -- two heartbeat bytes",
        [[0x00, 0x00, 0x00, 0x00, 0x55, 0x55, 0xA5 + i, 0xB1 - i]
         for i in range(40)],
        {6, 7},
    ),
    (
        "0x1A4  real signal wobbling 0x66<->0x67 must NOT be suppressed",
        # Byte 1 changes on only a few frames; byte 7 is the heartbeat.
        [[0x00, 0x66 + (1 if i % 12 == 0 else 0), 0x00, 0x00,
          0x00, 0x00, 0x00, (i * 0x0F) & 0xFF] for i in range(40)],
        {7},
    ),
    (
        "0x158  fully static payload plus heartbeat",
        [[0x00] * 7 + [(i * 0x0F) & 0xFF] for i in range(40)],
        {7},
    ),
]


def main():
    failures = 0
    for name, frames, expected in CASES:
        got = heartbeat_bytes(frames)
        ok = got == expected
        if not ok:
            failures += 1
        print(f"[{'PASS' if ok else 'FAIL'}] {name}")
        if not ok:
            print(f"         expected heartbeat bytes {sorted(expected)}, "
                  f"got {sorted(got)}")

    # Below the sample floor nothing may be judged -- otherwise a freshly seen
    # ID would have real signals suppressed before there is evidence.
    short = heartbeat_bytes(cycle([[0x00, 0x33], [0x00, 0x06]], 10))
    if short:
        failures += 1
        print(f"[FAIL] under {SNIFF_MIN_SAMPLES} samples nothing should be "
              f"judged, got {sorted(short)}")
    else:
        print(f"[PASS] under {SNIFF_MIN_SAMPLES} samples nothing is judged")

    print()
    print("FAILURES:", failures)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())

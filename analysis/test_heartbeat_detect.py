#!/usr/bin/env python3
"""Check the sniffer's heartbeat-byte heuristic against real Civic frames.

This tests the ALGORITHM, not the compiled firmware -- it is a faithful Python
port of isHeartbeatByte() in firmware/src/sniffer.cpp. If you change the C,
change this too.

Why it matters: the 2026-09-07 capture showed most Honda IDs carry a rolling
counter plus a checksum in their last byte. Without suppressing those, every
row in the sniffer table shows as "changed" and the tool is useless. The
heuristic is that a byte differing from the previous frame on nearly every
frame is a heartbeat, not a signal.

Payloads below are verbatim from the 2026-09-07 capture.
"""

SNIFF_MIN_SAMPLES = 20
SNIFF_HEARTBEAT_PCT = 90


def heartbeat_bytes(frames):
    """Port of isHeartbeatByte(). Returns the set of byte indices judged
    heartbeats after replaying `frames` (a list of equal-length byte lists)."""
    count = len(frames)
    if count < SNIFF_MIN_SAMPLES:
        return set()

    width = len(frames[0])
    changes = [0] * width
    for prev, cur in zip(frames, frames[1:]):
        for i in range(width):
            if cur[i] != prev[i]:
                changes[i] += 1

    comparisons = count - 1
    return {i for i in range(width)
            if (changes[i] * 100) // comparisons >= SNIFF_HEARTBEAT_PCT}


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

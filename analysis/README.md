# analysis

Python. Nothing here talks to the car — it reads the CSVs the board serves over
its access point.

## Getting a capture

Connect to the `cardiag` AP, open `http://192.168.4.1/`, then either download
`raw.csv` (the rolling PSRAM ring, roughly four minutes of every frame) or press
**START LOG**, drive, and download `changes.csv` (only frames where a
non-heartbeat byte moved).

## Finding out which byte is which

`diff_captures.py` answers one question: *which byte responds to the thing I
did?* You never need to know what a byte means to notice that it tracks the
brake pedal.

```sh
# two captures: one doing nothing, one doing the thing
python diff_captures.py baseline.csv brake.csv

# or one continuous capture, marking roughly when you did it
python diff_captures.py drive.csv --window 20000:26000
```

The window form is usually easier: record once, note when you pressed the thing,
and everything outside the window becomes the baseline.

Output is ranked by how strongly a byte moves under stimulus *and* stays still
at rest — a byte that twitches constantly in both is not a find. Rolling
counters and checksums are dropped by default because they move on every frame
and would otherwise fill the list; `--keep-heartbeats` brings them back, which
matters when a real signal only starts ticking during the action and therefore
looks like a counter.

`--csv` gives machine-readable output. `--top N` changes how many rows print.

Adjacent responding bytes on one ID are flagged as a likely 16-bit value rather
than two separate signals.

## Files

| | |
|---|---|
| `canlog.py` | Loading and per-byte statistics. Owns the Python side of the heartbeat rule. |
| `diff_captures.py` | The identification tool above. |
| `test_heartbeat_detect.py` | Pins the heartbeat rule against real Civic payloads. |
| `test_diff_captures.py` | End-to-end checks against synthetic captures with a planted signal. |

Run the tests directly; they need nothing installed.

```sh
python test_heartbeat_detect.py && python test_diff_captures.py
```

## Two things to know

**The heartbeat rule exists twice** — here in `canlog.py` and in
`firmware/src/sniffer.cpp` — because the firmware cannot import Python. Change
one, change the other. The tests pin the Python copy against payloads recorded
from the car.

**Raw and change captures are not comparable.** A change log only contains
frames where something moved, so its transition counts mean something different
from a raw capture's. `diff_captures.py` refuses to compare across the two
rather than silently ranking noise.

## Later

Baselining and anomaly detection — "coolant at 2500 rpm, twenty minutes in, is
normally 88–90 °C *on this car*" — need months of logs and a signal list. That
is Phase 3, and it starts once the tool above has produced the signal list.

"""Loading and per-byte statistics for cardiag CSV captures.

Handles both files the board produces:

    raw.csv      ms,id,ext,dlc,d0..d7
    changes.csv  ms,id,ext,dlc,changed,d0..d7

Cells past a frame's DLC are blank, so a short frame is never confused with one
carrying real zero bytes.

The heartbeat heuristic here mirrors isHeartbeatByte() in
firmware/src/sniffer.cpp. It is duplicated because the firmware cannot import
Python, not because two versions are wanted -- change one, change the other.
test_heartbeat_detect.py pins this copy against real captured payloads.
"""

from __future__ import annotations

import csv
from dataclasses import dataclass, field

# Must match firmware/include/config.h.
SNIFF_MIN_SAMPLES = 20
SNIFF_HEARTBEAT_PCT = 90


@dataclass
class Frame:
    ms: int
    id: int
    ext: bool
    dlc: int
    data: list[int]          # length == dlc
    changed: int | None = None   # mask, only present in changes.csv


def load(path: str) -> list[Frame]:
    """Reads a capture. Raises ValueError on a file that is not one of ours."""
    out: list[Frame] = []
    with open(path, newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        if reader.fieldnames is None or "id" not in reader.fieldnames:
            raise ValueError(f"{path}: not a cardiag capture (no id column)")
        has_changed = "changed" in reader.fieldnames

        for row in reader:
            dlc = int(row["dlc"])
            data = []
            for i in range(dlc):
                cell = row.get(f"d{i}", "")
                # A blank inside the DLC means a malformed row; skip the frame
                # rather than invent a zero.
                if cell == "" or cell is None:
                    data = []
                    break
                data.append(int(cell, 16))
            if len(data) != dlc:
                continue

            out.append(Frame(
                ms=int(row["ms"]),
                id=int(row["id"], 16),
                ext=row["ext"] == "1",
                dlc=dlc,
                data=data,
                changed=int(row["changed"], 16) if has_changed else None,
            ))
    return out


@dataclass
class ByteStats:
    """What one byte of one CAN ID did across a capture."""
    transitions: int = 0          # frames where it differed from the previous
    samples: int = 0              # frames seen
    values: set[int] = field(default_factory=set)
    first: int | None = None
    last: int | None = None

    @property
    def comparisons(self) -> int:
        return max(self.samples - 1, 0)

    @property
    def static(self) -> bool:
        return len(self.values) <= 1

    @property
    def change_rate(self) -> float:
        """Fraction of consecutive frames in which this byte moved."""
        return self.transitions / self.comparisons if self.comparisons else 0.0

    @property
    def heartbeat(self) -> bool:
        """Rolling counter or checksum rather than data.

        Mirrors isHeartbeatByte(): needs enough evidence before judging, or a
        freshly seen ID would have real signals suppressed.
        """
        if self.samples < SNIFF_MIN_SAMPLES:
            return False
        return self.change_rate * 100 >= SNIFF_HEARTBEAT_PCT

    @property
    def monotonic(self) -> bool:
        """Looks like a free-running counter: every observed value distinct and
        spanning most of the byte range. Weaker than `heartbeat`, and it catches
        slow counters that tick well under the heartbeat threshold."""
        return len(self.values) >= 200 and self.transitions >= len(self.values) - 1


def per_byte_stats(frames: list[Frame]) -> dict[tuple[int, int], ByteStats]:
    """Keyed by (can_id, byte_index).

    NOTE: a change-log capture only contains frames where something moved, so
    `transitions` is inflated relative to a raw capture of the same drive. That
    is fine for comparing two captures of the same KIND, and comparing a raw
    file against a change file is refused by the CLI for exactly this reason.
    """
    stats: dict[tuple[int, int], ByteStats] = {}
    last_payload: dict[int, list[int]] = {}

    for f in frames:
        prev = last_payload.get(f.id)
        for i, v in enumerate(f.data):
            st = stats.setdefault((f.id, i), ByteStats())
            st.samples += 1
            st.values.add(v)
            if st.first is None:
                st.first = v
            st.last = v
            if prev is not None and i < len(prev) and prev[i] != v:
                st.transitions += 1
        last_payload[f.id] = f.data

    return stats


def capture_kind(frames: list[Frame]) -> str:
    return "changes" if frames and frames[0].changed is not None else "raw"


def span_ms(frames: list[Frame]) -> tuple[int, int]:
    if not frames:
        return (0, 0)
    return (frames[0].ms, frames[-1].ms)

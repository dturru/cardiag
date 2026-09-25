"""soak_summary.py: heap judged per boot segment, not across reboots.

⚠️ SYNTHETIC CSVs, built here in the open. They are shaped like soak_wifi.py's
per-cycle CSV (same columns) but no run produced them -- they pin the
segmentation rule, which no captured run exercises on demand.

🐛 What changed and why these exist. The summary used to judge min free heap
across the whole run as though it "only ever falls and is NOT restored by a
reboot". It is a per-boot low-water mark: it restarts at every reset. So:

  * heap trends are judged INSIDE each boot segment,
  * a reboot row belongs to neither segment (its stats line may be from either
    side of the reset), and the next segment starts at the first post-boot row,
  * min free is reported as headroom, never failed on.
"""

from __future__ import annotations

import csv
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import soak_summary as ss  # noqa: E402

FIELDS = ["cycle", "detect_ms", "rejoin_ms", "fallback_ms", "drop_path",
          "reason", "heap", "minheap", "largest_block", "netstack_12308",
          "reboot", "reset_reason", "loop_max_us", "loop_max_stage",
          "loop_cycle_max_us", "loop_cycle_stage", "bus_idle_closes", "panics",
          "fs_sub_max_us", "fs_sub_stage", "fs_pass_us", "can_rx_missed",
          "can_rx_overrun", "can_chg_dropped", "can_id_overflow"]


def row(cycle: int, heap: int, *, minheap: int | None = None,
        largest: int | None = None, reboot: bool = False,
        reason: str = "") -> dict:
    return {"cycle": str(cycle), "detect_ms": "-1500.0", "rejoin_ms": "4000.0",
            "fallback_ms": "120", "drop_path": "event", "reason": "201",
            "heap": str(heap),
            "minheap": str(minheap if minheap is not None else heap - 20000),
            "largest_block": str(largest if largest is not None else heap // 2),
            "netstack_12308": "0", "reboot": "1" if reboot else "0",
            "reset_reason": reason,
            # A healthy loop and no false key-off closes, so these fixtures
            # judge heap and resets only; test_soak_verdict.py covers the rest.
            "loop_max_us": "42000", "loop_max_stage": "webui",
            "loop_cycle_max_us": "38000", "loop_cycle_stage": "webui",
            "bus_idle_closes": "0", "panics": "0",
            "fs_sub_max_us": "9000", "fs_sub_stage": "snapshot",
            "fs_pass_us": "12000", "can_rx_missed": "0", "can_rx_overrun": "0",
            "can_chg_dropped": "0", "can_id_overflow": "0"}


def write_csv(path: Path, rows: list[dict]) -> Path:
    with path.open("w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(rows)
    return path


def run_summary(tmp_path: Path, rows: list[dict]) -> str:
    c = write_csv(tmp_path / "soak.csv", rows)
    out = tmp_path / "SUMMARY.md"
    ss.main(["--csv", str(c), "--out", str(out), "--requested", str(len(rows))])
    return out.read_text(encoding="utf-8")


def read_back(tmp_path: Path, rows: list[dict]) -> list[dict]:
    c = write_csv(tmp_path / "soak.csv", rows)
    with c.open(encoding="utf-8", newline="") as fh:
        return list(csv.DictReader(fh))


# --- no reboot ---------------------------------------------------------------

def test_no_reboot_flat_heap_is_one_segment_and_passes(tmp_path):
    rows = [row(i, 200_000 + (i % 3) * 10) for i in range(1, 31)]
    j = ss.judge_heap(read_back(tmp_path, rows))
    assert len(j["segments"]) == 1
    assert j["segments"][0]["rows"] == 30
    assert j["fails"] == []
    assert "VERDICT: PASS" in run_summary(tmp_path, rows)


def test_no_reboot_leak_fails_inside_the_segment(tmp_path):
    rows = [row(i, 200_000 - i * 6300) for i in range(1, 21)]
    j = ss.judge_heap(read_back(tmp_path, rows))
    assert any("free heap trending down" in f for f in j["fails"])


def test_falling_min_free_alone_is_headroom_not_a_failure(tmp_path):
    # min free drifting down within one boot, with flat current heap: the old
    # summary failed this ("min free heap fell ..."); it is headroom.
    rows = [row(i, 200_000, minheap=180_000 - i * 400) for i in range(1, 31)]
    text = run_summary(tmp_path, rows)
    assert "VERDICT: PASS" in text
    assert "min free heap fell" not in text
    assert "headroom" in text


def test_fragmentation_fails_even_with_flat_free_heap(tmp_path):
    rows = [row(i, 200_000, largest=100_000 - i * 1000) for i in range(1, 21)]
    j = ss.judge_heap(read_back(tmp_path, rows))
    assert any("largest free block" in f for f in j["fails"])


# --- one reboot --------------------------------------------------------------

def test_one_reboot_splits_into_two_segments(tmp_path):
    # Flat before and after. The reset restores heap, which a whole-run slope
    # would read as a big positive jump; per segment there is no trend at all.
    rows = ([row(i, 150_000) for i in range(1, 16)]
            + [row(16, 210_000, reboot=True, reason="TASK_WDT")]
            + [row(i, 210_000) for i in range(17, 31)])
    j = ss.judge_heap(read_back(tmp_path, rows))
    assert [s["rows"] for s in j["segments"]] == [15, 14]
    assert j["fails"] == []
    # The reboot itself is still a failure of the run -- just not a heap one.
    text = run_summary(tmp_path, rows)
    assert "VERDICT: FAIL" in text and "1 reboot(s)" in text
    assert "TASK_WDT" in text


def test_leak_masked_by_a_reboot_is_still_caught(tmp_path):
    # Leaks 6.3 kB/cycle, resets, leaks again. A whole-run view sees heap go
    # back up; per segment both halves are clearly falling.
    rows = ([row(i, 210_000 - i * 6300) for i in range(1, 16)]
            + [row(16, 210_000, reboot=True, reason="PANIC")]
            + [row(i, 210_000 - (i - 16) * 6300) for i in range(17, 31)])
    j = ss.judge_heap(read_back(tmp_path, rows))
    assert sum("free heap trending down" in f for f in j["fails"]) == 2


def test_min_free_is_never_compared_across_segments(tmp_path):
    # Low min free in boot 1, high in boot 2: a per-boot mark restarting, not
    # "recovery", and not a fall either.
    rows = ([row(i, 200_000, minheap=90_000) for i in range(1, 12)]
            + [row(12, 200_000, reboot=True, reason="TASK_WDT")]
            + [row(i, 200_000, minheap=180_000) for i in range(13, 25)])
    j = ss.judge_heap(read_back(tmp_path, rows))
    assert [s["min_free"] for s in j["segments"]] == [90_000, 180_000]
    assert j["fails"] == []


# --- the boundary, off by one ------------------------------------------------

def test_reboot_row_belongs_to_neither_segment(tmp_path):
    rows = ([row(i, 100_000) for i in range(1, 6)]
            + [row(6, 999_999, reboot=True)]
            + [row(i, 200_000) for i in range(7, 11)])
    segs = ss.segments(read_back(tmp_path, rows))
    assert [r["cycle"] for r in segs[0]] == ["1", "2", "3", "4", "5"]
    # The first post-boot row is the one AFTER the reboot row.
    assert segs[1][0]["cycle"] == "7"
    assert all(r["cycle"] != "6" for s in segs for r in s)


def test_reboot_row_value_never_leaks_into_a_slope(tmp_path):
    # If the reboot row were counted on either side, its outlier heap would
    # dominate that segment's slope. Exactly MIN_SEGMENT_ROWS flat rows each
    # side, so both segments are judged and the boundary is what decides.
    n = ss.MIN_SEGMENT_ROWS
    rows = ([row(i, 150_000) for i in range(1, n + 1)]
            + [row(n + 1, 10_000, reboot=True)]
            + [row(i, 150_000) for i in range(n + 2, 2 * n + 2)])
    j = ss.judge_heap(read_back(tmp_path, rows))
    assert [s["rows"] for s in j["segments"]] == [n, n]
    assert [s["heap_slope"] for s in j["segments"]] == [0.0, 0.0]
    assert j["fails"] == []


def test_reboot_on_the_first_row_and_back_to_back(tmp_path):
    rows = ([row(1, 200_000, reboot=True)]
            + [row(2, 200_000, reboot=True)]
            + [row(i, 200_000) for i in range(3, 8)])
    segs = ss.segments(read_back(tmp_path, rows))
    assert len(segs) == 1 and segs[0][0]["cycle"] == "3"


def test_short_segment_is_shown_not_judged(tmp_path):
    rows = ([row(i, 200_000 - i * 9000) for i in range(1, 5)]  # 4 rows, steep
            + [row(5, 200_000, reboot=True)]
            + [row(i, 200_000) for i in range(6, 20)])
    j = ss.judge_heap(read_back(tmp_path, rows))
    assert j["segments"][0]["heap_slope"] < ss.HEAP_SLOPE_FAIL
    assert not any("segment 1" in f for f in j["fails"])

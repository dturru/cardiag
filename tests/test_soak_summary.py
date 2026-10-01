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

import pytest
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
          "can_rx_overrun", "can_chg_dropped", "can_id_overflow", "can_raw_busy"]


def row(cycle: int, heap: int, *, minheap: int | None = None,
        largest: int | None = None, reboot: bool = False,
        reason: str = "") -> dict:
    return {"cycle": str(cycle), "detect_ms": "-1500.0", "rejoin_ms": "4000.0",
            "fallback_ms": "120", "drop_path": "event", "reason": "201",
            "heap": str(heap),
            "minheap": str(minheap if minheap is not None else heap - 20000),
            # Flat and above LARGEST_FLOOR_B unless a test says otherwise.
            "largest_block": str(largest if largest is not None else 160_000),
            "netstack_12308": "0", "reboot": "1" if reboot else "0",
            "reset_reason": reason,
            # A healthy loop and no false key-off closes, so these fixtures
            # judge heap and resets only; test_soak_verdict.py covers the rest.
            "loop_max_us": "42000", "loop_max_stage": "webui",
            "loop_cycle_max_us": "38000", "loop_cycle_stage": "webui",
            "bus_idle_closes": "0", "panics": "0",
            "fs_sub_max_us": "9000", "fs_sub_stage": "snapshot",
            "fs_pass_us": "12000", "can_rx_missed": "0", "can_rx_overrun": "0",
            "can_chg_dropped": "0", "can_id_overflow": "0", "can_raw_busy": "0"}


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
    # 200 B/cycle for 130 cycles: 26 windows, envelope down ~25 KB, all above
    # the floor. Judged on the envelope (judge_largest), not by judge_heap.
    rows = [row(i, 200_000, largest=200_000 - i * 200) for i in range(1, 131)]
    rows = read_back(tmp_path, rows)
    assert ss.judge_heap(rows)["fails"] == []
    j = ss.judge(rows)
    assert j["largest"]["state"] == "leak"
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


# --- largest free block: upper envelope (ok / watch / leak) --------------------

UPPER, LOWER = 160_000, 160_000 - 8 * 1024        # two plateaus ~8 KB apart


def _flip_rows(n: int) -> list[dict]:
    """The 09-25 shape: largest_block alternating between two flat plateaus,
    in irregular runs, plus one isolated low sample. Must never alert."""
    rows = []
    for i in range(1, n + 1):
        v = UPPER if (i * 7) % 11 < 5 else LOWER     # bunched, irregular flips
        if i == n // 2:
            v = LOWER - 6000                         # one low sample
        rows.append(row(i, 200_000, largest=v))
    return rows


def _leak_rows(windows: int, step: int = 2048,
               w: int = ss.ENVELOPE_WINDOW_CYCLES) -> list[dict]:
    """Upper envelope stepping down ~2 KB per window, flipping inside each."""
    rows, i = [], 1
    for k in range(windows):
        top = UPPER - k * step
        for c in range(w):
            rows.append(row(i, 200_000, largest=top if c % 2 else top - 8 * 1024))
            i += 1
    return rows


def test_two_level_flip_is_ok():
    j = ss.judge(_flip_rows(240))                    # 48 windows
    assert j["largest"]["state"] == "ok", j["largest"]
    assert j["verdict"] == "PASS"
    seg = j["largest"]["segments"][0]
    assert set(seg["window_max"]) == {UPPER}         # the envelope is flat


def test_two_level_flip_never_alerts_at_any_phase():
    for shift in range(11):
        rows = _flip_rows(240 + shift)[shift:]
        assert ss.judge_largest(rows)["state"] == "ok", shift


def test_synthetic_leak_alerts():
    j = ss.judge(_leak_rows(12))                     # 11 steps x 2 KB = 22 KB
    lg = j["largest"]
    assert lg["state"] == "leak"
    assert lg["segments"][0]["decline_run"] == 11
    assert lg["segments"][0]["decline_b"] == 11 * 2048
    assert j["verdict"] == "FAIL"
    assert any("largest free block LEAK" in f for f in j["fails"])


def test_short_decline_is_watch_not_leak():
    # 9 windows: 8 steps x 2 KB = 16 KB, not > 16 KB -> enough windows, small fall.
    assert ss.ENVELOPE_K <= 8
    lg = ss.judge_largest(_leak_rows(9))
    assert lg["state"] == "watch"
    j = ss.judge(_leak_rows(9))
    assert j["verdict"] == "PASS"
    assert any("WATCH: largest free block" in ln for ln in ss.verdict_lines(j))


def test_big_fall_in_few_windows_is_watch():
    lg = ss.judge_largest(_leak_rows(4, step=8000))  # 3 steps (< K=4), 24 kB
    assert lg["state"] == "watch"


def test_floor_breach_is_leak_with_cycle():
    rows = [row(i, 200_000) for i in range(1, 31)]
    rows[11]["largest_block"] = str(100_000)
    lg = ss.judge_largest(rows)
    assert lg["state"] == "leak"
    assert lg["min_sample"] == 100_000 and lg["min_cycle"] == "12"
    assert "below the" in lg["reasons"][0]


def test_floor_is_configurable():
    rows = [row(i, 200_000, largest=110_000) for i in range(1, 31)]
    assert ss.judge_largest(rows)["state"] == "leak"
    assert ss.judge_largest(rows, floor_b=100_000)["state"] == "ok"


def test_envelope_is_judged_per_boot():
    """A reboot restores the heap: a fall across it is not a decline."""
    rows = _leak_rows(5) + [row(51, 200_000, reboot=True)]
    rows += [row(52 + i, 200_000, largest=UPPER - 9 * 2048) for i in range(25)]
    lg = ss.judge_largest(rows)
    assert lg["state"] != "leak"


def test_largest_in_summary_md(tmp_path):
    text = run_summary(tmp_path, _flip_rows(60))
    assert "largest free block:** **OK**" in text


# The real 09-25 soak, if its run folder is present locally (analysis/ is not
# all committed). Must not alert. Skipped in CI.
_ANALYSIS = Path(__file__).resolve().parents[1] / "analysis"
REAL = sorted(set(_ANALYSIS.glob("soak*2026-09-25*/*.csv"))
              | set(_ANALYSIS.glob("2026-09-25-final-bench-soak/**/soak.csv")))


@pytest.mark.skipif(not REAL, reason="09-25 soak CSV not present (local data)")
def test_real_0925_soak_does_not_alert():
    for p in REAL:
        rows = ss.read_rows(p)
        lg = ss.judge_largest(rows)
        assert lg is None or lg["state"] == "ok", (p.name, lg)


def test_a_40_cycle_soak_has_enough_windows_for_k():
    """09-25: 40 cycles at ~7 min. The defaults must be able to fire on it."""
    windows = 40 // ss.ENVELOPE_WINDOW_CYCLES
    assert windows - 1 >= ss.ENVELOPE_K
    lg = ss.judge_largest(_leak_rows(8, step=3 * 1024))   # 7 steps, 21 KB
    assert lg["segments"][0]["windows"] == 8
    assert lg["state"] == "leak"

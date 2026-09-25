"""One soak verdict: soak_summary.judge(), and everything agrees with it.

⚠️ SYNTHETIC CSVs, built here. Same columns as soak_wifi.py writes; no run
produced them. They pin the rules that three false PASSes got past:

  * a cycle with a loop() pass over 1 s FAILS, and says which cycle and stage,
  * a bus-idle close in SELFTEST FAILS (the loopback bus never goes quiet),
  * a check whose column is missing FAILS rather than passes,
  * SUMMARY.md, verdict.json (-> DONE) and the soak.log VERDICT line agree.
"""

from __future__ import annotations

import csv
import io
import json
import re
import sys
import types
from contextlib import redirect_stdout
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import soak_summary as ss  # noqa: E402

# soak_wifi imports pyserial at module load; the verdict path never touches it.
sys.modules.setdefault("serial", types.ModuleType("serial"))
import soak_wifi as sw  # noqa: E402

FIELDS = sw.CSV_FIELDS


def row(cycle: int, *, loop_cycle=38_000, stage="webui", loop_boot=42_000,
        boot_stage="webui", idle=0, panics=0, reboot=False, heap=200_000,
        missed=0, overrun=0, chg=0, idovf=0, rawbusy=0, fs_us=9_000,
        fs_stage="snapshot", fs_pass=12_000, fs_walks=0, logdrop=0):
    return {"cycle": cycle, "detect_ms": "-1500.0", "rejoin_ms": "9000.0",
            "fallback_ms": 120, "drop_path": "event", "reason": 201,
            "heap": heap, "minheap": heap - 20_000, "largest_block": heap // 2,
            "netstack_12308": 0, "reboot": int(reboot),
            "reset_reason": "TASK_WDT" if reboot else "",
            "loop_max_us": "" if loop_boot is None else loop_boot,
            "loop_max_stage": boot_stage,
            "loop_cycle_max_us": "" if loop_cycle is None else loop_cycle,
            "loop_cycle_stage": stage, "bus_idle_closes": idle,
            "panics": panics, "fs_sub_max_us": fs_us, "fs_sub_stage": fs_stage,
            "fs_pass_us": fs_pass, "fs_walks": fs_walks,
            "can_rx_missed": missed,
            "can_rx_overrun": overrun, "can_chg_dropped": chg,
            "can_id_overflow": idovf, "can_raw_busy": rawbusy,
            "log_dropped": logdrop}


def write(tmp_path: Path, rows: list[dict], fields=FIELDS) -> Path:
    p = tmp_path / "soak.csv"
    with p.open("w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)
    return p


def healthy(n=20):
    return [row(i) for i in range(1, n + 1)]


def verdict_of(tmp_path, rows, **kw) -> dict:
    return ss.judge(ss.read_rows(write(tmp_path, rows, **kw)),
                    requested=len(rows))


# --- the rules ------------------------------------------------------------------

def test_healthy_run_passes(tmp_path):
    j = verdict_of(tmp_path, healthy())
    assert j["verdict"] == "PASS", j["fails"]


def test_a_cycle_over_one_second_fails_naming_cycle_and_stage(tmp_path):
    rows = healthy()
    rows[6] = row(7, loop_cycle=1_200_000, stage="filestore",
                  loop_boot=1_200_000, boot_stage="filestore")
    j = verdict_of(tmp_path, rows)
    assert j["verdict"] == "FAIL"
    msg = " ".join(j["fails"])
    assert "cycle 7" in msg and "filestore" in msg and "1200 ms" in msg


def test_the_soak_that_passed_falsely_now_fails(tmp_path):
    # The #6 soak: max pass 3.74 s in filestore, 1,012 bus-idle closes over 40
    # cycles, no reboots, flat heap. It was reported PASS.
    rows = [row(i, loop_cycle=3_740_000 if i == 23 else 180_000,
                stage="filestore", loop_boot=3_740_000 if i >= 23 else 180_000,
                boot_stage="filestore", idle=25 + (i % 3))
            for i in range(1, 41)]
    j = verdict_of(tmp_path, rows)
    assert j["verdict"] == "FAIL"
    assert any("3740 ms in filestore" in f for f in j["fails"])
    assert any("bus-idle close" in f for f in j["fails"])


def test_since_boot_max_over_one_second_fails_without_a_cycle_column(tmp_path):
    # Older firmware: no loopwin=, only the since-boot loopmax=.
    rows = [row(i, loop_cycle=None, loop_boot=1_500_000 if i >= 5 else 50_000,
                boot_stage="filestore") for i in range(1, 21)]
    j = verdict_of(tmp_path, rows)
    assert j["verdict"] == "FAIL"
    assert any("cycle 5" in f and "since-boot" in f for f in j["fails"])
    # Counted once per boot, not once per remaining cycle.
    assert len(j["loop"]["over"]) == 1


def test_any_bus_idle_close_in_selftest_fails(tmp_path):
    rows = healthy()
    rows[3] = row(4, idle=1)
    j = verdict_of(tmp_path, rows)
    assert j["verdict"] == "FAIL"
    assert any("1 bus-idle close(s) in SELFTEST" in f and "4" in f
               for f in j["fails"])


def test_bus_idle_is_not_judged_outside_selftest(tmp_path):
    rows = healthy()
    rows[3] = row(4, idle=2)
    j = ss.judge(ss.read_rows(write(tmp_path, rows)), requested=len(rows),
                 mode="SNIFF")
    assert j["verdict"] == "PASS"


@pytest.mark.parametrize("missing", ["loop", "bus_idle_closes"])
def test_a_missing_metric_is_inconclusive_not_pass(tmp_path, missing):
    if missing == "loop":
        fields = [f for f in FIELDS
                  if f not in ("loop_max_us", "loop_cycle_max_us")]
    else:
        fields = [f for f in FIELDS if f != "bus_idle_closes"]
    j = verdict_of(tmp_path, healthy(), fields=fields)
    assert j["verdict"] == "INCONCLUSIVE"
    assert j["coverage_short"] == ["loop" if missing == "loop" else "bus_idle"]


def test_no_cycles_is_not_a_pass(tmp_path):
    j = ss.judge([], requested=0)
    assert j["verdict"] == "FAIL"


def test_panics_fail(tmp_path):
    rows = healthy()
    rows[0] = row(1, panics=1)
    assert verdict_of(tmp_path, rows)["verdict"] == "FAIL"


# --- SUMMARY, DONE and soak.log agree ------------------------------------------

CASES = {
    "pass": healthy(),
    "loop": healthy()[:9] + [row(10, loop_cycle=2_000_000, stage="hublink")]
            + healthy()[10:],
    "busidle": [row(i, idle=1 if i % 5 == 0 else 0) for i in range(1, 21)],
    "reboot": healthy()[:9] + [row(10, reboot=True)] + healthy()[10:],
    "partial": healthy(8),
}


@pytest.mark.parametrize("case", sorted(CASES))
def test_summary_verdict_file_and_log_line_agree(tmp_path, case):
    rows = CASES[case]
    requested = 20
    csv_path = write(tmp_path, rows)
    out, vfile = tmp_path / "SUMMARY.md", tmp_path / "verdict.json"
    rc = ss.main(["--csv", str(csv_path), "--out", str(out),
                  "--requested", str(requested), "--verdict-file", str(vfile)])

    summary = re.search(r"\*\*VERDICT: (\w+)\*\*", out.read_text("utf-8"))[1]
    done_verdict = json.loads(vfile.read_text("utf-8"))["verdict"]

    # soak.log: soak_wifi.report() prints the VERDICT line from the same CSV.
    args = types.SimpleNamespace(csv=str(csv_path), cycles=requested, dwell=20)
    buf = io.StringIO()
    with redirect_stdout(buf):
        wifi_rc = sw.report([], sw.Totals(), args)
    log = re.search(r"^VERDICT: (\w+)", buf.getvalue(), re.MULTILINE)[1]

    assert summary == done_verdict == log
    assert rc == wifi_rc == ss.EXIT[summary]
    expected = {"pass": "PASS", "partial": "PARTIAL"}.get(case, "FAIL")
    assert summary == expected


def test_run_soak_takes_done_from_the_verdict_file_only():
    """run_soak.ps1 can't run here; pin that it has no verdict of its own."""
    ps1 = (REPO / "tools" / "run_soak.ps1").read_text(encoding="utf-8")
    assert "'--verdict-file' $verdictFile" in ps1
    assert "ConvertFrom-Json).verdict" in ps1
    assert re.search(r'"PASS"\s*\{\s*Finish 0 "complete-pass" \$verdict', ps1)
    # The old path decided DONE from soak_wifi's exit code.
    assert 'if ($soakRc -eq 0) { Finish 0 "complete-pass" }' not in ps1
    assert "FINAL VERDICT (soak_summary.py" in ps1


# --- CAN drops and the filestore sub-stage ------------------------------------

@pytest.mark.parametrize("kw,what", [
    ({"missed": 3}, "rx_missed"),
    ({"overrun": 1}, "FIFO overrun"),
    ({"chg": 12}, "change-log ring full"),
    ({"idovf": 2}, "sniffer id table full"),
    ({"rawbusy": 1}, "raw ring busy"),
])
def test_any_can_drop_fails(tmp_path, kw, what):
    rows = healthy()
    # Counters are cumulative: once non-zero they stay so.
    rows = rows[:5] + [row(i, **kw) for i in range(6, 21)]
    j = verdict_of(tmp_path, rows)
    assert j["verdict"] == "FAIL"
    assert any(what in f and "first at cycle 6" in f for f in j["fails"])


def test_drop_counters_missing_is_inconclusive(tmp_path):
    fields = [f for f in FIELDS if not f.startswith("can_")]
    j = verdict_of(tmp_path, healthy(), fields=fields)
    assert j["verdict"] == "INCONCLUSIVE"
    assert "can_drops" in j["coverage_short"]


def test_fs_sub_stage_is_reported_not_failed(tmp_path):
    rows = [row(i, fs_us=600_000 if i % 4 == 0 else 20_000,
                fs_stage="fs_size" if i % 4 == 0 else "snapshot",
                fs_pass=800_000) for i in range(1, 21)]
    j = verdict_of(tmp_path, rows)
    assert j["verdict"] == "PASS"          # the loop check is the verdict
    us, cyc, st, _ = j["fs_sub"]["worst"]
    assert (us, st) == (600_000, "fs_size")
    assert j["fs_sub"]["stage_counts"] == {"fs_size": 5, "snapshot": 15}
    assert j["fs_sub"]["walks_total"] == 0
    out = tmp_path / "S.md"
    ss.main(["--csv", str(tmp_path / "soak.csv"), "--out", str(out),
             "--requested", "20"])
    assert "worst filestore sub-stage:** `fs_size` 600.0 ms" in out.read_text()


def test_stats_line_parses_into_the_cycle():
    line = ("[hublink] rejoin state=sta joins=3 drops=2(evt=2 poll=0) "
            "joinfail=0 apstarts=3 reason=201 fallback=120ms worst=150ms "
            "heap=200000 minheap=180000 largest=100000 loopmax=2400000us "
            "loopstage=filestore loopwin=1200000us loopwinstage=filestore "
            "fswin=1100000us fswinstage=fs_size fswinpass=1180000us fswalks=3 "
            "canmiss=0 canovr=0 chgdrop=4 idovf=0 rawbusy=0")
    tap = types.SimpleNamespace(snapshot=lambda: [sw.Line(0.0, line)])
    cyc, tot = sw.Cycle(n=1), sw.Totals()
    sw.scan_window(tap, 0, 1, cyc, tot)
    assert (cyc.fs_sub_max_us, cyc.fs_sub_stage, cyc.fs_pass_us) == (
        1_100_000, "fs_size", 1_180_000)
    assert (cyc.can_rx_missed, cyc.can_chg_dropped, cyc.can_raw_busy) == (0, 4, 0)
    assert cyc.fs_walks == 3
    assert (cyc.loop_cycle_max_us, cyc.loop_cycle_stage) == (1_200_000,
                                                             "filestore")


# --- scan-then-join ---------------------------------------------------------------

def test_failed_joins_are_counted_from_the_cumulative_counter(tmp_path):
    rows = healthy(10)
    fails = [0, 0, 1, 1, 1, 2, 2, 2, 2, 3]            # cumulative joinfail=
    for r, v in zip(rows, fails):
        r["join_fail"] = v
    fields = FIELDS + [f for f in ("join_fail",) if f not in FIELDS]
    j = verdict_of(tmp_path, rows, fields=fields)
    assert j["joins"]["failed_joins"] == 3
    assert j["verdict"] == "PASS"                      # reported, not failed on


def test_stats_line_scan_counters_parse():
    line = ("[hublink] rejoin state=sta joins=3 drops=2(evt=2 poll=0) "
            "joinfail=1 apstarts=3 reason=201 fallback=120ms scans=41 seen=3 "
            "scanfail=0 worst=150ms heap=200000 minheap=180000 largest=100000 "
            "loopmax=90000us loopstage=webui loopwin=80000us loopwinstage=webui")
    tap = types.SimpleNamespace(snapshot=lambda: [sw.Line(0.0, line)])
    cyc, tot = sw.Cycle(n=1), sw.Totals()
    sw.scan_window(tap, 0, 1, cyc, tot)
    assert (cyc.join_fail, cyc.scans, cyc.scan_seen) == (1, 41, 3)
    assert cyc.heap == 200_000


# --- coverage -------------------------------------------------------------------

def _garbled(r: dict) -> dict:
    """A cycle whose [stats] line was split: counters and sub-stage unread."""
    r = dict(r)
    for c in ("fs_sub_max_us", "fs_sub_stage", "fs_pass_us", "can_rx_missed",
              "can_rx_overrun", "can_chg_dropped", "can_id_overflow",
              "log_dropped"):
        r[c] = ""
    return r


def test_the_baseline_soak_shape_is_inconclusive(tmp_path):
    # 16 of 40 cycles with unreadable counters: 60 % coverage. It was PASS.
    rows = [_garbled(row(i)) if i % 5 in (0, 2) else row(i)
            for i in range(1, 41)]
    j = verdict_of(tmp_path, rows)
    assert j["coverage"]["can_drops"]["readable"] == 24
    assert j["verdict"] == "INCONCLUSIVE"
    assert set(j["coverage_short"]) == {"can_drops", "fs_sub", "log_drops"}


@pytest.mark.parametrize("unreadable,verdict", [(4, "PASS"), (5, "INCONCLUSIVE")])
def test_coverage_threshold_is_90_percent(tmp_path, unreadable, verdict):
    rows = [_garbled(row(i)) if i <= unreadable else row(i)
            for i in range(1, 41)]
    assert verdict_of(tmp_path, rows)["verdict"] == verdict   # 36/40 = 90 %


def test_a_failure_on_readable_cycles_still_fails(tmp_path):
    rows = [_garbled(row(i)) for i in range(1, 21)]
    rows[3] = row(4, loop_cycle=1_500_000, stage="filestore")
    j = verdict_of(tmp_path, rows)
    assert j["verdict"] == "FAIL"


def test_inconclusive_agrees_across_summary_verdict_file_and_log(tmp_path):
    rows = [_garbled(row(i)) if i % 2 else row(i) for i in range(1, 21)]
    csv_path = write(tmp_path, rows)
    out, vfile = tmp_path / "SUMMARY.md", tmp_path / "verdict.json"
    rc = ss.main(["--csv", str(csv_path), "--out", str(out),
                  "--requested", "20", "--verdict-file", str(vfile)])
    text = out.read_text("utf-8")
    assert re.search(r"\*\*VERDICT: (\w+)\*\*", text)[1] == "INCONCLUSIVE"
    assert "INCONCLUSIVE, not PASS" in text and "coverage" in text
    assert json.loads(vfile.read_text("utf-8"))["verdict"] == "INCONCLUSIVE"
    args = types.SimpleNamespace(csv=str(csv_path), cycles=20, dwell=20)
    buf = io.StringIO()
    with redirect_stdout(buf):
        wifi_rc = sw.report([], sw.Totals(), args)
    assert re.search(r"^VERDICT: (\w+)", buf.getvalue(), re.M)[1] == "INCONCLUSIVE"
    assert "coverage: can_drops readable on 10/20" in buf.getvalue()
    assert rc == wifi_rc == 4


def test_run_soak_maps_inconclusive():
    ps1 = (REPO / "tools" / "run_soak.ps1").read_text(encoding="utf-8")
    assert re.search(r'"INCONCLUSIVE"\s*\{\s*Finish 4 "inconclusive" \$verdict', ps1)


def test_counters_parse_from_their_own_stats_line():
    lines = [
        "[hublink] rejoin state=sta joins=3 drops=2(evt=2 poll=0) joinfail=0 "
        "apstarts=3 reason=201 fallback=120ms worst=150ms heap=200000 "
        "minheap=180000 largest=100000 loopmax=90000us loopstage=webui "
        "loopwin=80000us loopwinstage=webui",
        "[stats] fswin=40000us fswinstage=snapshot fswinpass=60000us "
        "canmiss=0 canovr=0 chgdrop=0 idovf=0 logdrop=12",
    ]
    tap = types.SimpleNamespace(
        snapshot=lambda: [sw.Line(0.0, t) for t in lines])
    cyc, tot = sw.Cycle(n=1), sw.Totals()
    sw.scan_window(tap, 0, 2, cyc, tot)
    assert (cyc.heap, cyc.loop_cycle_max_us) == (200_000, 80_000)
    assert (cyc.fs_sub_max_us, cyc.can_rx_missed, cyc.can_id_overflow) == (
        40_000, 0, 0)
    assert cyc.log_dropped == 12


# --- logdrop -----------------------------------------------------------------

def test_logdrop_is_reported_not_failed(tmp_path):
    # Since-boot counter: rises in cycles 5 and 12 only.
    rows = [row(i, logdrop=0 if i < 5 else 3 if i < 12 else 9)
            for i in range(1, 21)]
    j = verdict_of(tmp_path, rows)
    assert j["verdict"] == "PASS"
    assert j["logdrop"] == {"total": 9, "cycles": ["5", "12"]}


def test_coverage_accounts_for_logdrop_cycles(tmp_path):
    # Cycle 7 dropped lines and its counters were unread; cycle 3 was unread
    # with no drop. Both still count against coverage.
    rows = [row(i, logdrop=0 if i < 7 else 4) for i in range(1, 21)]
    rows[2] = _garbled(rows[2])
    rows[6] = dict(_garbled(rows[6]), log_dropped=4)
    j = verdict_of(tmp_path, rows)
    c = j["coverage"]["can_drops"]
    assert (c["readable"], c["in_logdrop_cycles"]) == (18, 1)


def test_summary_md_shows_logdrop(tmp_path):
    rows = [row(i, logdrop=0 if i < 5 else 2) for i in range(1, 21)]
    out = tmp_path / "SUMMARY.md"
    ss.main(["--csv", str(write(tmp_path, rows)), "--out", str(out)])
    text = out.read_text("utf-8")
    assert "serial lines dropped (log queue, not a failure):** 2 in 1 cycle(s)" in text

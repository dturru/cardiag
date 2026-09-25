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
        missed=0, overrun=0, chg=0, idovf=0, fs_us=9_000, fs_stage="snapshot",
        fs_pass=12_000, fs_walks=0):
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
            "can_id_overflow": idovf}


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
def test_a_missing_check_fails_rather_than_passes(tmp_path, missing):
    if missing == "loop":
        fields = [f for f in FIELDS
                  if f not in ("loop_max_us", "loop_cycle_max_us")]
    else:
        fields = [f for f in FIELDS if f != "bus_idle_closes"]
    j = verdict_of(tmp_path, healthy(), fields=fields)
    assert j["verdict"] == "FAIL"
    assert any("cannot be judged" in f for f in j["fails"])


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
])
def test_any_can_drop_fails(tmp_path, kw, what):
    rows = healthy()
    # Counters are cumulative: once non-zero they stay so.
    rows = rows[:5] + [row(i, **kw) for i in range(6, 21)]
    j = verdict_of(tmp_path, rows)
    assert j["verdict"] == "FAIL"
    assert any(what in f and "first at cycle 6" in f for f in j["fails"])


def test_drop_counters_missing_fails(tmp_path):
    fields = [f for f in FIELDS if not f.startswith("can_")]
    j = verdict_of(tmp_path, healthy(), fields=fields)
    assert j["verdict"] == "FAIL"
    assert any("CAN drop counters not reported" in f for f in j["fails"])


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
            "canmiss=0 canovr=0 chgdrop=4 idovf=0")
    tap = types.SimpleNamespace(snapshot=lambda: [sw.Line(0.0, line)])
    cyc, tot = sw.Cycle(n=1), sw.Totals()
    sw.scan_window(tap, 0, 1, cyc, tot)
    assert (cyc.fs_sub_max_us, cyc.fs_sub_stage, cyc.fs_pass_us) == (
        1_100_000, "fs_size", 1_180_000)
    assert (cyc.can_rx_missed, cyc.can_chg_dropped) == (0, 4)
    assert cyc.fs_walks == 3
    assert (cyc.loop_cycle_max_us, cyc.loop_cycle_stage) == (1_200_000,
                                                             "filestore")

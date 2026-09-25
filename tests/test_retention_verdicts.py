"""Regression tests for the bench harness's own verdict logic.

WHY THESE EXIST
---------------
The harness has now been wrong in BOTH directions on real hardware runs, and
each time it cost a bench session:

  2026-09-24 18:40  said "VERDICT: PASS (0 failed claims)" while claim 2 never
                    ran at all. Zero failures read as success.
  2026-09-24 19:01  said "** FAIL **" on a run where the firmware behaved
                    correctly -- it counted acked files of EVERY tier when
                    judging a Tier A cap eviction, and two Tier B snapshots
                    below the ack watermark turned a correct sequence into a
                    violation.

A harness that can lie in both directions is not a test, it is a coin flip. So
the three verdicts it can reach are pinned here against captured runs.

THE FIXTURES ARE GENERATED, NOT WRITTEN
---------------------------------------
tests/fixtures/*.json come from tools/make_verdict_fixture.py run against a
real analysis/bench-*/ folder (gitignored -- the raw run stays local, the
distilled fixture is committed). Do NOT hand-edit one to make a test pass;
regenerate it. The one exception is the synthetic violation below, which is a
NEGATIVE CONTROL: no run has ever produced it, and it is built in the open here
rather than posing as a capture.

The tests drive the real Watcher.record_eviction() and Watcher.ingest_row(),
not a reimplementation of them -- a test that recomputed the rule would pass
while the tool was broken, which is the exact failure being guarded against.
"""

from __future__ import annotations

import io
import json
import sys
from contextlib import redirect_stdout
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

from retention_watch import Watcher  # noqa: E402

FIXTURES = Path(__file__).resolve().parent / "fixtures"


def load(name: str) -> dict:
    return json.loads((FIXTURES / f"{name}.json").read_text(encoding="utf-8"))


def replay(fixture: dict) -> tuple[Watcher, str]:
    """Feed a fixture through the real watcher and capture its report."""
    w = Watcher(host="127.0.0.1", interval=3.0, ack_after=6, token="")
    w.baseline_lost = fixture.get("baseline_lost")

    rows = fixture["rows"]
    evictions = sorted(fixture["evictions"], key=lambda e: e["t"])
    ev_i = 0
    prev = None
    for row in rows:
        w.rows.append(row)
        w.ingest_row(row, prev)
        prev = row
        # Evictions are replayed in time order against the samples, the same
        # interleaving poll_files() produces live.
        while ev_i < len(evictions) and evictions[ev_i]["t"] <= row["t"]:
            e = evictions[ev_i]
            on_disk = {d["index"]: {"tier": d["tier"]} for d in e["on_disk"]}
            w.record_eviction(e["t"], e["index"], e["file"],
                              e["acked_through"], on_disk)
            ev_i += 1
    for e in evictions[ev_i:]:
        on_disk = {d["index"]: {"tier": d["tier"]} for d in e["on_disk"]}
        w.record_eviction(e["t"], e["index"], e["file"],
                          e["acked_through"], on_disk)

    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = w.report()
    w._rc = rc
    return w, buf.getvalue()


def claim_section(text: str, claim: str) -> str:
    """Just the block for one claim, so an assertion cannot match another."""
    marker = f"CLAIM {claim} --"
    start = text.index(marker)
    rest = text[start + len(marker):]
    end = rest.find("\nCLAIM ")
    return rest if end == -1 else rest[:end]


def summary_verdict(w: Watcher, tmp_path: Path) -> str:
    out = tmp_path / "SUMMARY.md"
    w.write_summary(str(out), getattr(w, "_rc", 0), {})
    for line in out.read_text(encoding="utf-8").splitlines():
        if line.startswith("**VERDICT:"):
            return line
    raise AssertionError("no VERDICT line in summary")


# --------------------------------------------------------------------------
# 1. The run that reported PASS while nothing under test ever ran.
# --------------------------------------------------------------------------

def test_1840_claim2_not_exercised(tmp_path):
    """18:40 -- the Tier A cap never fired, so claim 2 proved nothing."""
    w, text = replay(load("claim2_inconclusive_1840"))

    assert w.evicted == [], "fixture should contain no evictions"
    section = claim_section(text, "2")
    assert "NOT EXERCISED" in section
    assert "PASS" not in section
    assert "2" in w.unexercised_claims
    assert w.failures == 0, "nothing failed -- that is the trap"


def test_1840_summary_says_inconclusive_not_pass(tmp_path):
    """The headline must not read green when the claim never ran."""
    w, _ = replay(load("claim2_inconclusive_1840"))
    line = summary_verdict(w, tmp_path)

    assert "INCONCLUSIVE" in line
    assert "PASS" not in line
    assert "NOT EXERCISED" in line


# --------------------------------------------------------------------------
# 2. The run the harness called FAIL although the firmware was correct.
# --------------------------------------------------------------------------

def test_1901_claim2_passes(tmp_path):
    """19:01 -- six acked Tier A evicted first, then the unacked ones."""
    w, text = replay(load("claim2_pass_1901"))

    section = claim_section(text, "2")
    assert "PASS" in section
    assert "FAIL" not in section
    assert "2" not in w.unexercised_claims

    acked = [e for e in w.evicted if e["acked"]]
    unacked = [e for e in w.evicted if not e["acked"]]
    assert len(acked) == 6 and len(unacked) == 3
    assert max(e["t"] for e in acked) < min(e["t"] for e in unacked), \
        "every acked eviction must precede every unacked one"


def test_1901_acked_tier_b_does_not_count_against_the_tier_a_cap():
    """The exact false FAIL: #436 and #442 are Tier B, below the watermark.

    enforceTierACap() only ever chooses among Tier A, so an acked Tier B file
    on the disk is not a candidate it declined to take.
    """
    w, _ = replay(load("claim2_pass_1901"))
    first_unacked = next(e for e in w.evicted if not e["acked"])

    assert first_unacked["acked_available"] == 0, \
        "no acked TIER A file was left when the first unacked one went"
    assert first_unacked["acked_available_any"] > 0, \
        "acked files of other tiers WERE present -- the old rule saw these"


def test_1901_summary_says_pass(tmp_path):
    w, _ = replay(load("claim2_pass_1901"))
    line = summary_verdict(w, tmp_path)

    assert "PASS" in line
    assert "INCONCLUSIVE" not in line and "FAIL" not in line


# --------------------------------------------------------------------------
# 3. NEGATIVE CONTROL -- a real violation must still be caught.
#
# Built here in the open, not in tests/fixtures/: no hardware run has ever
# produced this, and a synthetic case posing as a capture is exactly what the
# fixture rule forbids. An unacked Tier A file is evicted at t=200 while an
# acked TIER A file (#10) is still sitting on the disk.
# --------------------------------------------------------------------------

def _synthetic_violation() -> dict:
    def row(t, dl_a, dl_u, usage=20):
        return {"t": t, "uptime_ms": int(t * 1000), "used": 800000,
                "total": 4063232, "usage_pct": usage, "warn": True,
                "warn_pct": 70, "files": 5, "open": 1, "acked_through": 10,
                "tier_a": 300000, "tier_b": 100000, "tier_c": 0,
                "pending_unacked": 2, "deleted_acked": dl_a,
                "deleted_unacked": dl_u, "lost_a": dl_u, "lost_b": 0,
                "lost_c": 0, "lost_bytes_a": dl_u * 65000, "lost_bytes_b": 0,
                "lost_bytes_c": 0, "lost_files_total": dl_u,
                "loss_recorded_files": 0, "tier_counters_present": True,
                "write_errors": 0, "rows_dropped": 0}

    return {
        "source_run": "SYNTHETIC -- negative control, not a capture",
        "baseline_lost": None,
        "rows": [row(100, 0, 0), row(200, 0, 1), row(300, 0, 1)],
        "evictions": [{
            "t": 200.0,
            "index": 11,                      # 11 > acked_through 10 => unacked
            "file": {"tier": "A", "kind": "changes", "bytes": 65600,
                     "synthetic": True},
            "acked_through": 10,
            # #10 is an ACKED TIER A file still on the disk. Taking #11 before
            # it is the violation.
            "on_disk": [{"index": 10, "tier": "A"},
                        {"index": 12, "tier": "B"}],
        }],
    }


def test_synthetic_unacked_before_acked_is_a_failure(tmp_path):
    w, text = replay(_synthetic_violation())

    section = claim_section(text, "2")
    assert "** FAIL **" in section
    assert w.failures >= 1

    ev = w.evicted[0]
    assert ev["acked"] is False
    assert ev["acked_available"] == 1, "one acked TIER A file was still there"

    line = summary_verdict(w, tmp_path)
    assert "FAIL" in line
    assert "PASS" not in line and "INCONCLUSIVE" not in line


def test_all_three_verdicts_are_reachable(tmp_path):
    """Belt and braces: the three outcomes must be distinguishable."""
    verdicts = set()
    for fixture in (load("claim2_inconclusive_1840"),
                    load("claim2_pass_1901"),
                    _synthetic_violation()):
        w, _ = replay(fixture)
        line = summary_verdict(w, tmp_path)
        verdicts.add(line.split("**")[1].replace("VERDICT:", "").strip())

    assert verdicts == {"PASS", "FAIL", "INCONCLUSIVE"}


@pytest.mark.parametrize("name", ["claim2_inconclusive_1840",
                                  "claim2_pass_1901"])
def test_fixtures_record_their_provenance(name):
    """A fixture that does not say where it came from cannot be regenerated."""
    fx = load(name)
    assert fx["source_run"].startswith("bench-")
    assert "regenerate" in fx["note"]

"""Watch the logger's storage while it fills, and judge the retention spec.

Protocol §2.3 makes four claims. This checks each one against the board rather
than against the source:

  1. deletion order is acked -> Tier A -> Tier B -> Tier C
  2. unacked data is removed only when there is nothing acked left to drop
  3. `storage.warn` appears on /api/v1/session before anything unacked is lost
  4. `deleted_unacked > 0` is reported, because it means the hub never
     collected data that is now gone

🔑 CLAIM 3 IS THE ONE WORTH WATCHING. `warn` is driven by TOTAL usage
(FS_WARN_USAGE_PCT, 70%), but Tier A is independently capped at
FS_TIER_A_MAX_PCT (40%) of the partition and that cap evicts on its own. If the
cap fires while total usage is still under 70%, an unacked Tier A file is
deleted with `warn` still false -- which is a real ordering gap, not a
rounding argument. This tool records `warn` at the instant the first unacked
deletion is observed, so the answer is a measurement.

    python tools/retention_watch.py --host 192.168.137.109 --seconds 600
    python tools/retention_watch.py --host ... --csv out.csv --interval 2

--- RUN 3: turning claim 2 into an actual test -------------------------------

Runs 1 and 2 could not test the deletion ORDER. Every file on the disk was
acked through index 3 and every Tier A file created during the fill was
unacked, so when the cap fired there was no acked-vs-unacked contest to
resolve, and claim 2 came back NOT EXERCISED -- honestly so.

`--ack-after N` fixes that: once N Tier A files exist it acks through the Nth
and leaves everything after it unacked, so that when the cap fires both kinds
are present and enforceTierACap() has to choose.

    python tools/retention_watch.py --host 192.168.137.109 --seconds 1800 \
        --ack-after 6 --token "$HUB_API_TOKEN" --csv retention3.csv

The ack lives in this tool rather than in a separate fill script on purpose:
the verdict is about the ORDER of acks and evictions, and one process watching
one timeline cannot disagree with itself about what happened first.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.request


def get(url: str, timeout: float = 6.0):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def post(url: str, payload: dict, token: str, timeout: float = 6.0):
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode("utf-8"), method="POST",
        headers={"Content-Type": "application/json", "X-Hub-Token": token})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


class Watcher:
    def __init__(self, host: str, interval: float,
                 ack_after: int = 0, token: str = ""):
        self.base = host if host.startswith("http") else f"http://{host}"
        self.interval = interval
        # Run 3: ack the Nth Tier A file once it exists, so the cap has both
        # acked and unacked Tier A to choose between. 0 disables (runs 1 & 2).
        self.ack_after = ack_after
        self.token = token
        self.acked_at: dict | None = None
        self.acked_index: int | None = None
        self.rows: list[dict] = []
        self.events: list[str] = []
        # The state at the moment each threshold was first crossed. None until
        # it happens, which is itself the answer to "did it happen at all".
        self.first_unacked_delete: dict | None = None
        self.first_warn: dict | None = None
        self.first_acked_delete: dict | None = None
        self.seen_indices: dict[int, dict] = {}
        # Eviction order as observed, so claim 2 is judged on the sequence and
        # not only on which counter moved first.
        self.evicted: list[dict] = []

    def poll(self) -> dict | None:
        try:
            s = get(f"{self.base}/api/v1/session")
        except Exception as exc:                      # noqa: BLE001
            self.events.append(f"session poll failed: {type(exc).__name__}")
            return None
        st = s.get("storage", {}) or {}
        row = {
            "t": round(time.monotonic() - self.t0, 2),
            "uptime_ms": s.get("uptime_ms"),
            "used": st.get("used"),
            "total": st.get("total"),
            "usage_pct": st.get("usage_pct"),
            "warn": bool(st.get("warn")),
            "warn_pct": st.get("warn_pct"),
            "files": st.get("files"),
            "open": st.get("open"),
            "acked_through": st.get("acked_through"),
            "tier_a": st.get("tier_a_bytes"),
            "tier_b": st.get("tier_b_bytes"),
            "tier_c": st.get("tier_c_bytes"),
            "deleted_acked": st.get("deleted_acked"),
            "deleted_unacked": st.get("deleted_unacked"),
            # Per-tier breakdown of the loss. Absent on firmware older than the
            # "never silent" fix; None is recorded rather than 0 so the CSV
            # cannot be read as "nothing was lost from that tier".
            "unacked_a": (st.get("unacked_evicted_files") or {}).get("A"),
            "unacked_b": (st.get("unacked_evicted_files") or {}).get("B"),
            "unacked_c": (st.get("unacked_evicted_files") or {}).get("C"),
            "unacked_bytes_a": (st.get("unacked_evicted_bytes") or {}).get("A"),
            "unacked_bytes_b": (st.get("unacked_evicted_bytes") or {}).get("B"),
            "unacked_bytes_c": (st.get("unacked_evicted_bytes") or {}).get("C"),
            "tier_counters_present":
                isinstance(st.get("unacked_evicted_files"), dict),
            "write_errors": st.get("write_errors"),
            "rows_dropped": st.get("rows_dropped"),
        }
        self.rows.append(row)
        return row

    def maybe_ack(self) -> None:
        """Run 3: ack a prefix of Tier A once enough files exist.

        Acks THROUGH the Nth Tier A file by index, which leaves every later
        file unacked. That is the state runs 1 and 2 never reached and the only
        one in which the cap's choice is observable.
        """
        if not self.ack_after or self.acked_at is not None:
            return
        tier_a = sorted(i for i, f in self.seen_indices.items()
                        if f.get("tier") == "A" and f.get("closed", True))
        if len(tier_a) < self.ack_after:
            return
        through = tier_a[self.ack_after - 1]
        try:
            res = post(f"{self.base}/api/v1/files/ack",
                       {"through_index": through}, self.token)
        except Exception as exc:                      # noqa: BLE001
            self.events.append(f"ACK FAILED ({type(exc).__name__}) -- run 3 "
                               f"cannot test ordering without it")
            self.ack_after = 0                        # do not spin on it
            return
        self.acked_index = int(res.get("acked_through", through))
        self.acked_at = dict(self.rows[-1]) if self.rows else {}
        t = self.acked_at.get("t", 0)
        self.events.append(
            f"[t={t:7.1f}] ACKED through #{self.acked_index} "
            f"({self.ack_after} Tier A files); everything later stays unacked")

    def poll_files(self) -> None:
        try:
            files = get(f"{self.base}/api/v1/files")
        except Exception:                             # noqa: BLE001
            return
        now = {f["index"]: f for f in files}
        # ⚠️ REFRESH, do not only insert. A file is first seen as an OPEN .part
        # and only later closes; caching the first sighting left every Tier A
        # file permanently recorded as closed=false, so --ack-after never found
        # a candidate and run 3 could not arm. The eviction record below wants
        # the LAST known state anyway -- final size, not the size when the file
        # was first noticed.
        for idx, f in now.items():
            self.seen_indices[idx] = f
        gone = [i for i in self.seen_indices if i not in now]
        for i in gone:
            f = self.seen_indices.pop(i)
            t = self.rows[-1]["t"] if self.rows else 0
            # Whether THIS file was acked, judged against the watermark in
            # force. `acked_through` is a watermark, so anything at or below it
            # was collected -- that is what makes the order checkable.
            wm = self.rows[-1].get("acked_through") if self.rows else None
            was_acked = wm is not None and i <= wm
            self.evicted.append({"t": t, "index": i, "tier": f.get("tier"),
                                 "bytes": f.get("bytes"), "acked": was_acked})
            self.events.append(
                f"[t={t:7.1f}] EVICTED "
                f"#{i} tier={f.get('tier')} kind={f.get('kind')} "
                f"bytes={f.get('bytes')} acked={was_acked} "
                f"synthetic={f.get('synthetic')}")

    def run(self, seconds: float) -> None:
        self.t0 = time.monotonic()
        prev = None
        while time.monotonic() - self.t0 < seconds:
            row = self.poll()
            if row is not None:
                self.poll_files()
                self.maybe_ack()
                if row["warn"] and self.first_warn is None:
                    self.first_warn = dict(row)
                    self.events.append(f"[t={row['t']:7.1f}] WARN set at "
                                       f"usage {row['usage_pct']}%")
                if prev is not None:
                    if (row["deleted_acked"] or 0) > (prev["deleted_acked"] or 0) \
                            and self.first_acked_delete is None:
                        self.first_acked_delete = dict(row)
                        self.events.append(
                            f"[t={row['t']:7.1f}] first ACKED deletion, "
                            f"usage {row['usage_pct']}%")
                    if (row["deleted_unacked"] or 0) > (prev["deleted_unacked"] or 0) \
                            and self.first_unacked_delete is None:
                        self.first_unacked_delete = dict(row)
                        self.events.append(
                            f"[t={row['t']:7.1f}] *** first UNACKED deletion, "
                            f"usage {row['usage_pct']}%, warn={row['warn']} ***")
                prev = row
                print(f"[{row['t']:7.1f}] used {row['used']:>9}/{row['total']} "
                      f"({row['usage_pct']:>3}%) warn={str(row['warn']):<5} "
                      f"files={row['files']:<3} A={row['tier_a']:>8} "
                      f"B={row['tier_b']:>8} "
                      f"del a/u={row['deleted_acked']}/{row['deleted_unacked']}",
                      flush=True)
            time.sleep(self.interval)

    # --- verdicts -----------------------------------------------------------

    def report(self) -> int:
        print("\n" + "=" * 72)
        print("RETENTION WATCH")
        print("=" * 72)
        for e in self.events:
            print("  " + e)
        if not self.rows:
            print("  no samples -- the board was not reachable")
            return 3

        first, last = self.rows[0], self.rows[-1]
        print(f"\nusage      {first['usage_pct']}% -> {last['usage_pct']}%")
        print(f"tier A     {first['tier_a']} -> {last['tier_a']} B")
        print(f"tier B     {first['tier_b']} -> {last['tier_b']} B")
        print(f"tier C     {first['tier_c']} -> {last['tier_c']} B")
        print(f"deleted    acked {last['deleted_acked']}  "
              f"unacked {last['deleted_unacked']}")
        if last["tier_counters_present"]:
            print(f"lost/tier  A {last['unacked_a']} files "
                  f"/ {last['unacked_bytes_a']} B   "
                  f"B {last['unacked_b']} / {last['unacked_bytes_b']} B   "
                  f"C {last['unacked_c']} / {last['unacked_bytes_c']} B")
        print(f"write err  {last['write_errors']}   rows dropped "
              f"{last['rows_dropped']}")

        failures = 0
        print("\nCLAIM 3 -- warn before any unacked loss:")
        if self.first_unacked_delete is None:
            print("  NOT EXERCISED: no unacked file was deleted in this run.")
        elif self.first_unacked_delete["warn"]:
            print(f"  PASS: warn was already set when the first unacked file "
                  f"went (usage {self.first_unacked_delete['usage_pct']}%).")
        else:
            failures += 1
            print(f"  ** FAIL **: an unacked file was deleted at usage "
                  f"{self.first_unacked_delete['usage_pct']}% with warn=False.")
            print(f"     warn fires at {last['warn_pct']}% of the WHOLE "
                  f"partition, but the Tier A cap evicts on its own share, so "
                  f"it can fire first. The loss IS reported "
                  f"(deleted_unacked={self.first_unacked_delete['deleted_unacked']}), "
                  f"it is simply not pre-warned.")

        print("\nCLAIM 3b -- the loss is attributed to a tier:")
        if self.first_unacked_delete is None:
            print("  NOT EXERCISED: nothing unacked was deleted.")
        elif not last["tier_counters_present"]:
            failures += 1
            print("  ** FAIL **: unacked data was destroyed and the session "
                  "reports no per-tier breakdown. The hub is told THAT data "
                  "went, not WHAT -- and Tier A and Tier B do not cost the "
                  "same. (Firmware predates the 'never silent' fix?)")
        else:
            counted = sum(int(last[k] or 0)
                          for k in ("unacked_a", "unacked_b", "unacked_c"))
            total = int(last["deleted_unacked"] or 0)
            if counted == total:
                print(f"  PASS: all {total} unacked deletion(s) are attributed "
                      f"(A={last['unacked_a']} B={last['unacked_b']} "
                      f"C={last['unacked_c']}).")
            else:
                failures += 1
                print(f"  ** FAIL **: deleted_unacked={total} but the per-tier "
                      f"counters sum to {counted}. One eviction path is not "
                      f"going through the shared accounting.")

        # Claim 2 is judged on the observed SEQUENCE, not on which counter
        # moved first. The counters only say how many of each went; the
        # sequence says whether an unacked file was destroyed while an acked
        # one was still sitting there, which is the actual claim.
        print("\nCLAIM 2 -- acked goes before unacked:")
        first_unacked_ev = next((e for e in self.evicted if not e["acked"]), None)
        acked_after_unacked = [
            e for e in self.evicted
            if e["acked"] and first_unacked_ev and e["t"] > first_unacked_ev["t"]]
        if not self.evicted:
            print("  NOT EXERCISED: nothing was deleted.")
        elif first_unacked_ev is None:
            print("  NOT EXERCISED as an ordering test: only acked files were "
                  "dropped, which is the correct end of the order.")
        elif not self.ack_after and not self.first_acked_delete:
            print("  NOT EXERCISED as an ordering test: there were no acked "
                  "files to drop, so going straight to unacked is correct.")
            print("     Re-run with --ack-after N to create the contest.")
        elif acked_after_unacked:
            failures += 1
            print(f"  ** FAIL **: unacked #{first_unacked_ev['index']} went at "
                  f"t={first_unacked_ev['t']:.1f}, but "
                  f"{len(acked_after_unacked)} acked file(s) were still on "
                  f"disk and were only deleted afterwards "
                  f"(#{acked_after_unacked[0]['index']} at "
                  f"t={acked_after_unacked[0]['t']:.1f}).")
        else:
            n_acked = sum(1 for e in self.evicted if e["acked"])
            print(f"  PASS: {n_acked} acked file(s) were exhausted before "
                  f"unacked #{first_unacked_ev['index']} was touched.")
        return 1 if failures else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--seconds", type=float, default=600.0)
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--csv")
    ap.add_argument("--ack-after", type=int, default=0, metavar="N",
                    help="run 3: once N closed Tier A files exist, ack through "
                         "the Nth and leave the rest unacked, so the Tier A "
                         "cap has both kinds to choose between")
    ap.add_argument("--token", default=os.environ.get("HUB_API_TOKEN", ""),
                    help="X-Hub-Token for /api/v1/files/ack "
                         "(default: $HUB_API_TOKEN)")
    args = ap.parse_args(argv)

    if args.ack_after and not args.token:
        print("--ack-after needs --token (or $HUB_API_TOKEN); /files/ack is "
              "authenticated and a 401 would look like a retention result.",
              file=sys.stderr)
        return 2

    w = Watcher(args.host, args.interval,
                ack_after=args.ack_after, token=args.token)
    try:
        w.run(args.seconds)
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
    rc = w.report()

    if args.csv and w.rows:
        import csv as _csv
        with open(args.csv, "w", newline="", encoding="utf-8") as fh:
            wr = _csv.DictWriter(fh, fieldnames=list(w.rows[0].keys()))
            wr.writeheader()
            wr.writerows(w.rows)
        print(f"\nper-sample CSV -> {args.csv}")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())

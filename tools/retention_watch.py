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
        # Lifetime loss total observed BEFORE the partition was erased, so
        # claim 5 can assert the NVS record outlived the wipe.
        self.baseline_lost: int | None = None
        self.failures = 0
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

    def open_csv(self, path: str) -> None:
        """Open the per-sample CSV for INCREMENTAL writing.

        ⭐ It used to be written once at the end, inside main(), so a killed run
        left a .log and no .csv at all. An unattended run that gets cut is the
        normal case, not the exception, so every sample is now written and
        flushed as it is taken: a kill leaves a short but complete CSV.
        """
        import csv as _csv
        self._csv_path = path
        self._csv_fh = open(path, "w", newline="", encoding="utf-8")
        self._csv_writer = None
        self._csv_mod = _csv

    def _csv_row(self, row: dict) -> None:
        if getattr(self, "_csv_fh", None) is None:
            return
        if self._csv_writer is None:
            self._csv_writer = self._csv_mod.DictWriter(
                self._csv_fh, fieldnames=list(row.keys()))
            self._csv_writer.writeheader()
        self._csv_writer.writerow(row)
        self._csv_fh.flush()

    def close_csv(self) -> None:
        if getattr(self, "_csv_fh", None) is not None:
            self._csv_fh.close()
            self._csv_fh = None

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
            "pending_unacked": st.get("pending_unacked"),
            # SINCE BOOT, RAM, reset by every reboot.
            "deleted_acked": st.get("deleted_acked"),
            "deleted_unacked": st.get("deleted_unacked"),
            # LIFETIME, NVS, monotonic. Absent on firmware older than the
            # "never silent" fix; None is recorded rather than 0 so the CSV
            # cannot be read as "nothing was lost from that tier".
            # ⚠️ These and the pair above are DIFFERENT BASES -- never compare.
            "lost_a": (st.get("lost_files") or {}).get("A"),
            "lost_b": (st.get("lost_files") or {}).get("B"),
            "lost_c": (st.get("lost_files") or {}).get("C"),
            "lost_bytes_a": (st.get("lost_bytes") or {}).get("A"),
            "lost_bytes_b": (st.get("lost_bytes") or {}).get("B"),
            "lost_bytes_c": (st.get("lost_bytes") or {}).get("C"),
            "lost_files_total": st.get("lost_files_total"),
            "loss_recorded_files": st.get("loss_recorded_files"),
            "tier_counters_present": isinstance(st.get("lost_files"), dict),
            "write_errors": st.get("write_errors"),
            "rows_dropped": st.get("rows_dropped"),
        }
        self.rows.append(row)
        self._csv_row(row)
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
            # 🔑 How many ACKED files were still on disk AT THIS MOMENT.
            #
            # Claim 2 is "unacked goes only when nothing acked is left", and
            # that has to be judged against the watermark IN FORCE WHEN THE
            # DELETION HAPPENED. Ack status is not a property of a file, it is
            # a property of a file at a time: a mid-run ack turns dozens of
            # files from unacked to acked at once. Comparing a later acked
            # eviction against an earlier unacked one without this produces a
            # false FAIL -- those files were not acked yet when the earlier
            # one went.
            acked_available = sum(
                1 for j in self.seen_indices
                if wm is not None and j <= wm and j != i)
            self.evicted.append({"t": t, "index": i, "tier": f.get("tier"),
                                 "bytes": f.get("bytes"), "acked": was_acked,
                                 "acked_available": acked_available})
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
            print(f"lost/tier  A {last['lost_a']} files "
                  f"/ {last['lost_bytes_a']} B   "
                  f"B {last['lost_b']} / {last['lost_bytes_b']} B   "
                  f"C {last['lost_c']} / {last['lost_bytes_c']} B   "
                  f"(LIFETIME; hub recorded {last['loss_recorded_files']} "
                  f"of {last['lost_files_total']})")
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
            # ⚠️ Compared against lost_files_total, NOT deleted_unacked. Those
            # are different bases -- lifetime NVS vs since-boot RAM -- and they
            # are SUPPOSED to disagree after any reboot. Comparing them would
            # make this check fail every time the board restarts.
            counted = sum(int(last[k] or 0)
                          for k in ("lost_a", "lost_b", "lost_c"))
            total = int(last["lost_files_total"] or 0)
            if counted == total:
                print(f"  PASS: all {total} lifetime loss(es) are attributed "
                      f"(A={last['lost_a']} B={last['lost_b']} "
                      f"C={last['lost_c']}).")
            else:
                failures += 1
                print(f"  ** FAIL **: lost_files_total={total} but the per-tier "
                      f"counters sum to {counted}. One eviction path is not "
                      f"going through the shared accounting.")

        # Claim 2 is judged on the observed SEQUENCE, and on the watermark that
        # was IN FORCE at each deletion -- never on a later one. A mid-run ack
        # reclassifies dozens of files at once, so "an acked file was deleted
        # after this unacked one" is not evidence of anything by itself.
        #
        # The violation is narrow and specific: an unacked file was destroyed
        # WHILE an acked file was sitting on the disk AT THAT MOMENT.
        print("\nCLAIM 2 -- acked goes before unacked:")
        unacked_evs = [e for e in self.evicted if not e["acked"]]
        violations = [e for e in unacked_evs if e["acked_available"] > 0]
        if not self.evicted:
            print("  NOT EXERCISED: nothing was deleted.")
        elif not unacked_evs:
            print("  NOT EXERCISED as an ordering test: only acked files were "
                  "dropped, which is the correct end of the order.")
        elif violations:
            failures += 1
            v = violations[0]
            print(f"  ** FAIL **: unacked #{v['index']} (tier {v['tier']}) was "
                  f"deleted at t={v['t']:.1f} while {v['acked_available']} "
                  f"acked file(s) were still on the disk.")
        elif not any(e["acked"] for e in self.evicted):
            print("  NOT EXERCISED as an ordering test: no acked file was ever "
                  "on the disk to compete, so going straight to unacked is "
                  "correct behaviour, not a failure.")
            print("     Re-run with --ack-after N to create the contest.")
        else:
            n_acked = sum(1 for e in self.evicted if e["acked"])
            print(f"  PASS: every unacked deletion happened with no acked file "
                  f"left on the disk, and {n_acked} acked file(s) were taken "
                  f"in preference once they existed.")

        # ⭐ THE HEADLINE TEST OF THE CLEAN-PARTITION RUN.
        #
        # Runs 1-3 could never reach this: the bench partition was Tier B
        # dominant and sat at 87-91%, so `warn` was true from total usage the
        # whole time and the second arm was never under test. On an empty
        # partition Tier A grows into its 40% cap while TOTAL usage is still
        # well under warn_pct -- so a loss here must set warn on its own.
        print("\nCLAIM 4 -- warn fires on loss even when usage is LOW:")
        wp = int(last["warn_pct"] or 70)
        low_loss = [r for r in self.rows
                    if (r["usage_pct"] or 0) < wp
                    and (r["lost_files_total"] or 0) > (r["loss_recorded_files"] or 0)]
        if not low_loss:
            print(f"  NOT EXERCISED: no sample had unrecorded loss while usage "
                  f"was under {wp}%. Either the Tier A cap never fired, or the "
                  f"partition filled past {wp}% first -- check `usage` above. "
                  f"This is the run's whole point, so a NOT EXERCISED here "
                  f"means the setup needs revisiting, not that the code is ok.")
        elif all(r["warn"] for r in low_loss):
            r = low_loss[0]
            print(f"  PASS: warn was TRUE at usage {r['usage_pct']}% (< {wp}%) "
                  f"with {r['lost_files_total']} lifetime loss(es) and only "
                  f"{r['loss_recorded_files']} recorded. The second arm works "
                  f"on its own -- total usage was not carrying it.")
        else:
            failures += 1
            r = next(r for r in low_loss if not r["warn"])
            print(f"  ** FAIL **: at usage {r['usage_pct']}% (< {wp}%), "
                  f"lost_files_total={r['lost_files_total']} exceeded "
                  f"loss_recorded_files={r['loss_recorded_files']} and warn was "
                  f"FALSE. Unrecorded data loss is not raising the warning.")

        print("\nCLAIM 5 -- the loss record survived the partition erase:")
        if self.baseline_lost is None:
            print("  NOT CHECKED: no --baseline-lost given.")
        elif (first["lost_files_total"] or 0) >= self.baseline_lost:
            print(f"  PASS: lifetime loss read {first['lost_files_total']} at "
                  f"the start of this run, against {self.baseline_lost} recorded "
                  f"before the spiffs partition was erased. NVS is a separate "
                  f"partition and the counters are not reset by wiping the "
                  f"filesystem -- which is the closest thing to a format.")
        else:
            failures += 1
            print(f"  ** FAIL **: lifetime loss was {self.baseline_lost} before "
                  f"the erase and {first['lost_files_total']} after. The record "
                  f"did not survive, so 'never reset, including by a format' is "
                  f"not true.")

        self.failures = failures
        return 1 if failures else 0

    def write_summary(self, path: str, rc: int, meta: dict) -> None:
        """PASS/FAIL plus the numbers, in a file, separate from the log.

        Required by the uninterruptible-work rule: the run must leave a verdict
        on disk that can be read without re-deriving it from a 2,000-line log.
        """
        last = self.rows[-1] if self.rows else {}
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("# Bench run summary\n\n")
            fh.write(f"**VERDICT: {'PASS' if rc == 0 else 'FAIL'}** "
                     f"({getattr(self, 'failures', '?')} failed claim(s))\n\n")
            for k, v in meta.items():
                fh.write(f"- **{k}:** {v}\n")
            fh.write(f"- **samples:** {len(self.rows)}\n")
            if last:
                fh.write(f"- **final usage:** {last.get('usage_pct')}% "
                         f"(warn_pct {last.get('warn_pct')})\n")
                fh.write(f"- **warn at end:** {last.get('warn')}\n")
                fh.write(f"- **lifetime loss:** {last.get('lost_files_total')} "
                         f"file(s) — A={last.get('lost_a')} B={last.get('lost_b')} "
                         f"C={last.get('lost_c')}; hub recorded "
                         f"{last.get('loss_recorded_files')}\n")
                fh.write(f"- **since boot:** deleted_acked="
                         f"{last.get('deleted_acked')} deleted_unacked="
                         f"{last.get('deleted_unacked')} "
                         f"(different basis — do not compare to the line above)\n")
                fh.write(f"- **write errors:** {last.get('write_errors')} · "
                         f"**rows dropped:** {last.get('rows_dropped')}\n")
            fh.write(f"- **evictions observed:** {len(self.evicted)}\n\n")
            fh.write("## Events\n\n")
            for e in self.events:
                fh.write(f"- {e}\n")
            fh.write("\n## Full verdicts\n\nSee `retention.log` "
                     "(the RETENTION WATCH banner at the end).\n")
            fh.write("\n## Serial\n\n`serial.log` in this folder holds the "
                     "board's own output for the same window, including the "
                     "boot line with the reset reason if it rebooted.\n")


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
    ap.add_argument("--summary", metavar="PATH",
                    help="write a PASS/FAIL summary file here")
    ap.add_argument("--baseline-lost", type=int, metavar="N",
                    help="lifetime lost-file total observed BEFORE the spiffs "
                         "partition was erased; enables claim 5 (the NVS loss "
                         "record must survive a filesystem wipe)")
    args = ap.parse_args(argv)

    if args.ack_after and not args.token:
        print("--ack-after needs --token (or $HUB_API_TOKEN); /files/ack is "
              "authenticated and a 401 would look like a retention result.",
              file=sys.stderr)
        return 2

    w = Watcher(args.host, args.interval,
                ack_after=args.ack_after, token=args.token)
    w.baseline_lost = args.baseline_lost
    # Opened BEFORE the run, not written after it: an unattended run that gets
    # killed must still leave a usable CSV of everything up to that moment.
    if args.csv:
        w.open_csv(args.csv)
    try:
        w.run(args.seconds)
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
    finally:
        w.close_csv()
    rc = w.report()

    if args.csv:
        print(f"\nper-sample CSV -> {args.csv}")
    if args.summary:
        w.write_summary(args.summary, rc, {
            "host": args.host,
            "seconds requested": args.seconds,
            "interval": args.interval,
            "ack-after": args.ack_after or "(none)",
            "baseline lifetime loss (pre-erase)":
                args.baseline_lost if args.baseline_lost is not None else "(not given)",
        })
        print(f"summary -> {args.summary}")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())

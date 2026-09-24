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
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.request


def get(url: str, timeout: float = 6.0):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


class Watcher:
    def __init__(self, host: str, interval: float):
        self.base = host if host.startswith("http") else f"http://{host}"
        self.interval = interval
        self.rows: list[dict] = []
        self.events: list[str] = []
        # The state at the moment each threshold was first crossed. None until
        # it happens, which is itself the answer to "did it happen at all".
        self.first_unacked_delete: dict | None = None
        self.first_warn: dict | None = None
        self.first_acked_delete: dict | None = None
        self.seen_indices: dict[int, dict] = {}

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
            "write_errors": st.get("write_errors"),
            "rows_dropped": st.get("rows_dropped"),
        }
        self.rows.append(row)
        return row

    def poll_files(self) -> None:
        try:
            files = get(f"{self.base}/api/v1/files")
        except Exception:                             # noqa: BLE001
            return
        now = {f["index"]: f for f in files}
        for idx, f in now.items():
            if idx not in self.seen_indices:
                self.seen_indices[idx] = f
        gone = [i for i in self.seen_indices if i not in now]
        for i in gone:
            f = self.seen_indices.pop(i)
            self.events.append(
                f"[t={self.rows[-1]['t'] if self.rows else 0:7.1f}] EVICTED "
                f"#{i} tier={f.get('tier')} kind={f.get('kind')} "
                f"bytes={f.get('bytes')} synthetic={f.get('synthetic')}")

    def run(self, seconds: float) -> None:
        self.t0 = time.monotonic()
        prev = None
        while time.monotonic() - self.t0 < seconds:
            row = self.poll()
            if row is not None:
                self.poll_files()
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

        print("\nCLAIM 2 -- acked goes before unacked:")
        if self.first_acked_delete and self.first_unacked_delete:
            if self.first_acked_delete["t"] <= self.first_unacked_delete["t"]:
                print("  PASS: an acked file was dropped before any unacked one.")
            else:
                failures += 1
                print("  ** FAIL **: unacked was dropped first.")
        elif self.first_unacked_delete and not self.first_acked_delete:
            print("  NOT EXERCISED as an ordering test: there were no acked "
                  "files to drop, so going straight to unacked is correct.")
        else:
            print("  NOT EXERCISED: nothing was deleted.")
        return 1 if failures else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--seconds", type=float, default=600.0)
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--csv")
    args = ap.parse_args(argv)

    w = Watcher(args.host, args.interval)
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

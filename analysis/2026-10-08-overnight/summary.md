# Overnight SELFTEST soak — 2026-10-08 (cardiag master `64fe27b`, default build)

Started 01:17 (esptool reset → boot 136, SELFTEST). Checked 11:28.

## Logger — PASS
- boot_id 136 throughout (10.2 h at 11:28), crashes_total 20 → 20.
- heap free ~176–177 k flat, min 145,060 (reached by ~2 h, flat after); loop_stack free_min 3,856; mdns_stack_min 1,644; parked/reaped 0/0.
- CAN running, **0 bus-off recoveries all night** (earlier SELFTEST nights: bus-off at 1.6–5.5 h).
- Sync: acked_through 12949 = hub watermark, pending 0, deleted_unacked 0, `[sync]` every ~6 min, 0 quarantined.

## Harness problems (not the logger)
- `serial.log` stalled at 01:21:50 (3,754,554 B), process alive. Cause unknown (possibly a console freeze); stopped by hand 11:31. No serial for the night.
- From ~06:44 mDNS stopped working on the laptop hotspot: laptop could resolve neither `cardiag.local` nor `carhub.local`, the Pi's avahi saw only itself. `overnight.log`: 65 good polls to 06:41, then 57 UNREACHABLE (name-only poller). Unicast kept working; the hub synced on its cached address.
- **Confirmed hotspot multicast:** `hotspot.ps1 stop/start` at 11:32 → both names resolve again (logger re-leased at .9).

## Hotspot restart = Wi-Fi drop test (11:32)
- Logger link_stats: sta_drops 1, join_failures 1, ap_starts 2 (fallback AP 21 ms), sta_joins 2; boot 136 kept, crashes 20.
- mdns parked/reaped still 0/0 ⇒ the link-drop leak path did NOT park a task; still unexercised.
- Hub re-resolved cardiag.local → .9, sync continued (acked 12950, pending 0).

## Soak 2 (11:31 10-08 → 08:30 10-09) — FAIL
Ended by hand 10-09 ~08:35 (poller pid 18092 + watchdog pid 54648 stopped; hotspot left On). `serial2.log` ran its full 12 h and ended 23:32 with 0 crash lines, so there's **no serial coverage of the failures below**. Final full snapshot: `final-session.json` (logger at .77).

| Time | Event |
|---|---|
| 11:31 → 23:52 | boot 136 clean: heap min 145,060 → 139,668, stack 3,856, 0 recoveries |
| 23:52 | first **bus-off** (TEC 128) after ~23 h with none; recoveries 12 → 96 by 01:10 |
| 00:14 | watchdog starts restarting the hotspot every ~5 min (91 restarts until 08:26) |
| 01:16–01:31 | **5 reboots (boot 136 → 141), crashes 20 → 23**, last_crash_reason `TASK_WDT`, no dump present (dumps_acked 7) |
| 01:21 → 08:30 | boot 141 stable, but `pkts=0` all night (SELFTEST traffic gone after reboot), 0 recoveries |

- Sync OK: `pending_unacked 0`, acked_through 13113, `open 0`, `leftover_parts 0`. The 18 files queued after the 19:44–21:27 outage are synced.
- **Watchdog bug (PR #36):** it logged "cardiag.local unresolved" on 91 checks in a row while the poller resolved `cardiag.local` on most of those polls ⇒ its probe gives false negatives and it bounced the hotspot all night. Boot 141 link_stats: sta_drops 76 / ap_starts 155. `loop.max` is 4.05 s in stage `webui`.
- Hub IP moved .194 → .205 (hotspot re-leases).
- UNVERIFIED: that the hotspot churn caused the TASK_WDT reboots (the timing fits, but there's no serial), and what caused the 23:52 bus-off onset.

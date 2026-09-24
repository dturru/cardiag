# Bench run summary

**VERDICT: PASS** (0 failed claim(s))

- **host:** 192.168.137.147
- **seconds requested:** 1200.0
- **interval:** 3.0
- **ack-after:** 6
- **baseline lifetime loss (pre-erase):** 34
- **samples:** 343
- **final usage:** 30% (warn_pct 70)
- **warn at end:** True
- **lifetime loss:** 34 file(s) — A=14 B=20 C=0; hub recorded 16
- **since boot:** deleted_acked=0 deleted_unacked=0 (different basis — do not compare to the line above)
- **write errors:** 0 · **rows dropped:** 0
- **evictions observed:** 0

## Events

- [t=    0.1] WARN set at usage 0%
- [t=  519.2] ACKED through #404 (6 Tier A files); everything later stays unacked

## Full verdicts

See `retention.log` (the RETENTION WATCH banner at the end).

## Serial

`serial.log` in this folder holds the board's own output for the same window, including the boot line with the reset reason if it rebooted.

## Reset / watchdog lines found in serial.log

```
[   1.785] [fs] LIFETIME LOSS RECORD: 34 file(s) of uncollected data destroyed (A=14 B=20 C=0); hub has recorded 16.  *** WARN STAYS SET ***
[   2.678] E (2997) task_wdt: esp_task_wdt_init(517): TWDT already initialized
[   2.678] E (6230) task_wdt: esp_task_wdt_reset(707): task not found
[   2.678] [wdt] task watchdog armed, 30s (no hardware failsafe on the TCAN1043G -- this is the only one that survives a hang)
```

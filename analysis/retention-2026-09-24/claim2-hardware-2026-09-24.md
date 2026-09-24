# Claim 2 on hardware — 2026-09-24

Board: ESP32-CAN-X2 on COM3, `esp32-can-x2-fstest`, Wi-Fi 192.168.137.64.
Partition state: Tier B dominant (~3.1 MB of 4.06 MB), usage 87-89%.

## What was tested

Protocol 2.3 claim 2: **unacked data is removed only when there is nothing
acked left to drop.**

Runs 1 and 2 reported this NOT EXERCISED, honestly: nothing acked remained on
the disk, so going straight to unacked was correct behaviour rather than a
failure. The contest was created here by acking a broad prefix mid-run.

## Method

1. Board filling Tier A (SELFTEST + change log) with retention active.
2. Baseline observed: `acked_through=3`, `deleted_acked=0`,
   `deleted_unacked` climbing steadily (7 -> 26 over the session).
   Every eviction was unacked, because nothing acked was left.
3. Acked through the highest closed index:
   `POST /api/v1/files/ack {"through_index":259}` -> `{"ok":true,
   "acked_through":259}`. Everything on the disk is now acked; everything
   written from here on is not.
4. Polled `/api/v1/session` every 6 s and watched the two counters.

## Result — PASS

| t | acked_through | deleted_acked | deleted_unacked | per-tier unacked |
|---|---|---|---|---|
| baseline | 3 | 0 | 26 | A:7 B:19 C:0 |
| after ack | 259 | 0 | 26 | A:7 B:19 C:0 |
| +~30 s | 259 | **1** | 26 | A:7 B:19 C:0 |
| +~60 s | 259 | **2** | 26 | A:7 B:19 C:0 |

**`deleted_acked` moved 0 -> 1 -> 2 while `deleted_unacked` stayed at 26.**

Before the ack the board was destroying unacked data at roughly one file per
20 s. The moment acked files existed, retention took those instead and did not
touch a single unacked file. The two counters move independently and in the
right order.

## What this does NOT show

- **`enforceTierACap()` was never entered.** Tier A never exceeded ~70 kB
  against a ~1.6 MB cap, because the partition is Tier B dominant. What ran was
  `enforceRetention()`'s 90% rule calling `evictOne()`. So this validates the
  GLOBAL deletion order, not the cap's internal acked-before-unacked choice,
  which is what section 1b of the handoff is specifically about.
- **The new `warn` arm is still unproven.** Usage sat at 87-89%, above the 70%
  threshold, so `warn` was true from total usage throughout. The fix's second
  arm -- warn set by an unacked eviction while usage is LOW -- needs a clean
  partition to exercise.

Both need a fresh partition. See `docs/NEXT-SESSION.md` section 1a.

## Also verified this session

Per-tier attribution is exact: at `deleted_unacked=6`, the counters read
`{A:4, B:2}` and summed to 6. The Tier B component was 131,256 bytes -- the
irreplaceable class, and completely invisible in the old single total.

---

# 🚨 FINDING — the loss counters do not survive a reboot

Found in run 3's raw rows, not by looking for it. `del a/u` read `0/22` at
t=8.8 and `0/0` from t≈288 onward, with `files=56` and Tier A still growing:
the board rebooted mid-run and the counters started again from zero.

## Confirmed in the source

`filestoreBegin()` does `memset(&g_st, 0, sizeof(g_st))` and then restores
only two values from NVS:

```
g_st.nextIndex    = g_fsPrefs.getULong("idx", 0);
g_st.ackedThrough = (int32_t)g_fsPrefs.getLong("ack", -1);
```

`deletedAcked`, `deletedUnacked` and the new `unackedEvictedBytes/Files` are
RAM only. **Every boot zeroes them.**

## Why this matters more than it looks

Protocol 2.3 names `deleted_unacked` as "the field to act on", and the 09-24
fix makes `warn` latch on it (`deletedUnacked > 0`). Both of those guarantees
are erased by a power cycle — and **in the field the board power-cycles at
every key-off**, because the transceiver's INH pin removes power when the bus
sleeps.

So the sequence that loses the evidence is the NORMAL one:

1. Long trip. Retention destroys unacked data. `deleted_unacked` goes above
   zero and `warn` latches true.
2. The hub does not poll before the bus goes quiet — which is the expected
   case, because a trip's files sync at the NEXT ignition (protocol 2.3.1).
3. Key-off. Power drops. Counters and the latched warning are gone.
4. Next ignition: the logger reports a clean `deleted_unacked = 0`.

**The data is still missing, and nothing says so.** The hub can infer a gap
from the index sequence, but "never silent" was supposed to mean it is told.

## NOT yet decided — this is a design call, deliberately left open

Persisting to NVS costs flash-wear on a path that can fire repeatedly while
the disk is full, so it is not an obvious yes. Options, roughly in order of
appeal:

1. **Persist on the transition only** — write when `deletedUnacked` goes from
   0 to non-zero, not on every eviction. One write per boot at most, which is
   what the warning actually needs; the exact counts stay volatile.
2. Persist counters on clean key-off, alongside the existing file close.
   Cheap, but loses exactly the crash case the counter exists for.
3. Leave volatile and have the HUB own the record: it already receives
   `logger_storage` on hub/health, so it could keep a per-device cumulative
   total. Costs nothing on the logger, but only works for losses the hub was
   awake to hear about — which is the same hole.

⇒ (1) looks right: the durable thing should be "data was lost since this
device was last serviced", not the running count.

## Reboot cause — UNCONFIRMED

Two candidates, not distinguished:
- The task watchdog fired during eviction. The board was running the fstest
  build flashed BEFORE the 09-24 WDT fix, so the retention path fed nothing,
  and the disk was at 87-89% with eviction running constantly. This is
  exactly section 2's hypothesis.
- An unrelated crash.

Next run should capture serial alongside the HTTP poll: the firmware prints
the reset reason on boot, which settles it in one line. If it was the
watchdog, that is independent confirmation that the section 2 fix was needed.

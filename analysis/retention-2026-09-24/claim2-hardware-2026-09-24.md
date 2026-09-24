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

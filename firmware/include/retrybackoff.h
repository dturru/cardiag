#pragma once

// ---------------------------------------------------------------------------
// When to look for the hub again, as a pure schedule, so test/test_retrybackoff
// can pin it.
//
// 🐛 WHY (2026-09-25 soak, master 1c23efb). After a drop the board retried
// once at 10 s and then fell to a FIXED 60 s cadence. When the hotspot came
// back, that single 10 s probe usually landed before the hub was answering,
// failed, and the next look was a full minute later: median rejoin 57.8 s on
// all 40 cycles, against 9 s before.
//
// Now: 5, 10, 20, 40, then 60 s (the cap) between attempts, doubling after
// each failed join. The schedule RESTARTS on every STA disconnect, because a
// fresh drop is the moment the hub is most likely to come straight back; a
// hub that stays away settles to the 60 s cadence within ~75 s, so it still
// does not cost an AP teardown every few seconds.
// ---------------------------------------------------------------------------

#include <stdint.h>

struct RetryBackoff {
  uint32_t firstMs;   // first delay after a reset
  uint32_t capMs;     // never wait longer than this
  uint8_t  step;      // failed attempts since the last reset
};

static inline void backoffInit(RetryBackoff *b, uint32_t firstMs,
                               uint32_t capMs) {
  b->firstMs = firstMs;
  b->capMs = capMs;
  b->step = 0;
}

// An STA disconnect happened: start the schedule again from firstMs.
static inline void backoffReset(RetryBackoff *b) { b->step = 0; }

// The delay before the next attempt, and advance the schedule. Doubling,
// computed in 64 bits and saturating at capMs, so step can grow without limit
// and never overflow into a short wait.
static inline uint32_t backoffNextMs(RetryBackoff *b) {
  uint64_t d = b->firstMs;
  for (uint8_t i = 0; i < b->step && d < b->capMs; i++) d <<= 1;
  if (d > b->capMs) d = b->capMs;
  if (b->step < 255) b->step++;
  return (uint32_t)d;
}

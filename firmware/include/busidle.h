#pragma once

// ---------------------------------------------------------------------------
// "Has the bus been quiet long enough?" -- the clean key-off test, as a pure
// function so test/test_busidle can drive it with the exact values that broke.
//
// 🐛 WHY (2026-09-25, soak on #6). filestoreLoop() cached `now = millis()` at
// the top of the pass, did the snapshot write and the change-log drain, then
// compared `(uint32_t)(now - g_lastBusMs)`. g_lastBusMs is written by canTask
// on every received frame, on the other core. A frame that arrived during the
// pass made last NEWER than the cached now, the unsigned difference wrapped to
// ~49.7 days, and every file was closed as if the key had been turned off:
// 1,012 false closes in 40 cycles, each one lengthening the pass.
//
// The rule, for every millis() subtraction against a timestamp another task
// writes: read the shared value ONCE into a local, and compare SIGNED. A
// timestamp from the future is "0 ms ago", never "49 days ago".
// ---------------------------------------------------------------------------

#include <stdint.h>

// Milliseconds since `last`, as seen at `now`. 0 if `last` is 0 (never seen)
// or newer than `now` (written by another task after `now` was sampled).
static inline uint32_t busQuietMs(uint32_t now, uint32_t last) {
  if (!last) return 0;
  const int32_t d = (int32_t)(now - last);
  return d > 0 ? (uint32_t)d : 0;
}

// True when a bus that was once active has been silent for >= thresholdMs.
// A board that has never seen a frame is never "idle": absence of traffic
// nobody looked for is not evidence the trip is over.
static inline bool busIdleFor(uint32_t now, uint32_t last,
                              uint32_t thresholdMs) {
  return last != 0 && busQuietMs(now, last) >= thresholdMs;
}

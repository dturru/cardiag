#pragma once

// ---------------------------------------------------------------------------
// Single-producer / single-consumer ring INDICES, lock-free on the producer.
//
// WHY (2026-09-25). canTask appended to the change log under the recorder
// mutex (portMAX_DELAY) -- the same mutex the filestore drain held while
// formatting 2 KB of CSV, and compaction held while memmoving up to 1.5 MB of
// PSRAM. On a busy bus that is longer than the 128-frame TWAI RX queue lasts
// (~28 ms at 4,500 frames/s), and a blocked canTask is a dropped frame.
//
// Now the producer (canTask) only ever reads `tail` and writes `head`, with
// acquire/release ordering, and never waits for anything: a full ring is a
// counted drop, not a block. Everything on the consumer side -- the drain, a
// CSV download, discarding drained rows, a clear -- serialises among itself
// with its own lock, which the producer never touches. "Compaction" is now
// advancing `tail`: O(1), no memmove.
//
// Indices are free-running 32-bit counters; `cap` must be a power of two so
// `index & (cap - 1)` stays continuous across the 2^32 wrap. Header-only:
// test/test_spscring drives it from two threads.
// ---------------------------------------------------------------------------

#include <atomic>
#include <stdint.h>

struct SpscRing {
  std::atomic<uint32_t> head{0};   // next slot to write; producer only
  std::atomic<uint32_t> tail{0};   // oldest unconsumed; consumer only
  uint32_t cap = 0;                // power of two, or 0 = disabled
};

static inline uint32_t spscPow2Floor(uint32_t n) {
  uint32_t p = 1;
  while (n && p <= n / 2) p <<= 1;
  return n ? p : 0;
}

static inline void spscInit(SpscRing *r, uint32_t cap) {
  r->cap = spscPow2Floor(cap);
  r->head.store(0, std::memory_order_relaxed);
  r->tail.store(0, std::memory_order_relaxed);
}

// --- producer -----------------------------------------------------------------

// A free slot to fill, or false when full (count it as a drop). Never waits.
static inline bool spscReserve(SpscRing *r, uint32_t *slot) {
  if (!r->cap) return false;
  const uint32_t h = r->head.load(std::memory_order_relaxed);
  const uint32_t t = r->tail.load(std::memory_order_acquire);
  if (h - t >= r->cap) return false;
  *slot = h & (r->cap - 1);
  return true;
}

// Make the slot from spscReserve() visible to the consumer.
static inline void spscPublish(SpscRing *r) {
  r->head.store(r->head.load(std::memory_order_relaxed) + 1,
                std::memory_order_release);
}

// --- consumer (callers serialise among themselves) ---------------------------

static inline uint32_t spscCount(const SpscRing *r) {
  return r->head.load(std::memory_order_acquire) -
         r->tail.load(std::memory_order_relaxed);
}

// Slot of the i-th unconsumed row (0 = oldest). Valid while i < spscCount().
static inline uint32_t spscSlot(const SpscRing *r, uint32_t i) {
  return (r->tail.load(std::memory_order_relaxed) + i) & (r->cap - 1);
}

// Give the oldest n rows back to the producer.
static inline uint32_t spscConsume(SpscRing *r, uint32_t n) {
  const uint32_t have = spscCount(r);
  if (n > have) n = have;
  r->tail.store(r->tail.load(std::memory_order_relaxed) + n,
                std::memory_order_release);
  return n;
}

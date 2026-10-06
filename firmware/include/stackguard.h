#pragma once

// ---------------------------------------------------------------------------
// Loop-task stack margin. main.cpp samples the loop task's high-water mark
// (uxTaskGetStackHighWaterMark: bytes never used) once a second; this keeps
// the lowest value seen and says ONCE when it first drops below the margin, so
// the serial log gets one warning line, not one a second.
//
// Why: bench 4 (2026-10-05). handleSession's 3.5-4.5 kB stack buffer plus the
// poll-plan hash's 1.5 kB canonical buffer and newlib's printf overflowed the
// 8 kB loop stack on every GET /api/v1/session (PR #30 heap-debug build: stack
// canary). Without the canary the overflow lands silently in the heap below
// the stack. On /api/v1/session as "loop_stack".
// ---------------------------------------------------------------------------

#include <stdint.h>

#define STACKGUARD_MARGIN_BYTES 1024u

struct StackGuard {
  uint32_t minFree;       // lowest high-water mark seen; UINT32_MAX = none yet
  bool     warned;
};

static inline void stackGuardInit(StackGuard *g) {
  g->minFree = UINT32_MAX;
  g->warned = false;
}

// Record a sample. True exactly once: the first sample below `margin`.
static inline bool stackGuardSample(StackGuard *g, uint32_t freeBytes,
                                    uint32_t margin) {
  if (freeBytes < g->minFree) g->minFree = freeBytes;
  if (!g->warned && freeBytes < margin) {
    g->warned = true;
    return true;
  }
  return false;
}

#pragma once

// ---------------------------------------------------------------------------
// HEAP NUMBERS WITHOUT WALKING THE HEAP PER REQUEST, and (debug builds) a
// periodic integrity check that names the first corruption it sees.
//
// heap_caps_get_largest_free_block() WALKS every block of every heap it is
// asked about. GET /api/v1/session did that twice per request, and on PR #29
// that walk is where an already-corrupted internal heap was found (LoadProhibited
// in tlsf_walk_pool, backtrace through handleSession). The walk was the
// detector, not the cause -- but a request handler is the worst place to
// detect it. Now loop() samples every HEAP_SAMPLE_MS and the handlers report
// the cached numbers.
//
// CARDIAG_HEAP_CHECK=1 (the heapdebug envs) adds heap_caps_check_integrity_all
// every HEAP_CHECK_PERIOD_MS. The FIRST failure is logged with its time, the
// last good check and the last mDNS actions, and kept in RTC memory so the
// next boot prints it even if the board panics before the hub reads it.
// ---------------------------------------------------------------------------

#include <stdint.h>

struct HeapSample {
  uint32_t atMs;              // millis() of the sample; 0 = none yet
  uint32_t freeInternal;      // heap_caps_get_free_size(INTERNAL)
  uint32_t minFreeInternal;   // low-water mark since boot
  uint32_t largestInternal;   // the walks, done here and only here
  uint32_t largest8bit;
  int32_t  mdnsStackFree;     // mdns task stack high-water mark, bytes; -1 = no task
  int32_t  mdnsStackFreeMin;  // lowest seen this boot; -1 = never seen
};

struct HeapCheckState {
  bool     enabled;           // built with CARDIAG_HEAP_CHECK
  uint32_t checks;            // integrity checks run this boot
  uint32_t failures;          // ... that failed
  uint32_t lastOkMs;          // millis() of the last passing check
  uint32_t firstFailMs;       // 0 = never failed this boot
  uint32_t maxCheckUs;        // slowest check
  char     firstFailMdns[48]; // last mDNS actions at the first failure
  // From the PREVIOUS boot, if it recorded a failure (RTC memory).
  bool     prevValid;
  uint32_t prevBootId, prevFailMs, prevLastOkMs;
  char     prevMdns[48];
};

void heapdiagBegin();                     // setup(): print a previous boot's record
void heapdiagLoop();                      // loop(): sample, and check if enabled
const HeapSample     *heapdiagSample();
const HeapCheckState *heapdiagCheck();

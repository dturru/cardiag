#pragma once

// ---------------------------------------------------------------------------
// When to LOOK for the hub, instead of when to TRY it.
//
// WHY (2026-09-25 soak on 3086fce). The 5/10/20/40/60 s backoff brought the
// median rejoin from 57.8 s to 30.6 s, but every attempt was a blind join:
// tear down the AP, try for 8 s, bring the AP back. The hotspot's AP comes up
// 8-18 s after the hotspot "returns", so the first attempts landed before it
// existed -- 1.7 failed joins per cycle, max rejoin 80 s -- and each failure
// cost the fallback AP ~10 s of downtime.
//
// Now the board keeps its AP up and runs an async scan (AP+STA, the STA side
// only ever scans) on this schedule, and joins the moment the hub's SSID is
// in a result. Failed joins should become rare; they are still counted.
//
//   * every fastMs (4 s) from the last STA disconnect or the last sighting,
//   * every slowMs (30 s) once slowAfterMs (2 min) have passed without one,
//   * every scan is directed at the hub's SSID; while its channel is known,
//     only that channel is scanned (~100 ms off the AP's channel instead of a
//     full sweep), with a full sweep every `fullEvery` scans in case it moved.
//
// Header-only, clock injected: test/test_scansched.
// ---------------------------------------------------------------------------

#include <stdint.h>

struct ScanSched {
  uint32_t fastMs, slowMs, slowAfterMs;
  uint8_t  fullEvery;      // every Nth scan sweeps all channels
  uint32_t searchSinceMs;  // last disconnect, or last sighting
  uint32_t lastScanMs;
  uint32_t scans;          // since the last reset
  bool     scannedOnce;
};

static inline void scanSchedInit(ScanSched *s, uint32_t fastMs, uint32_t slowMs,
                                 uint32_t slowAfterMs, uint8_t fullEvery) {
  s->fastMs = fastMs;
  s->slowMs = slowMs;
  s->slowAfterMs = slowAfterMs;
  s->fullEvery = fullEvery ? fullEvery : 1;
  s->searchSinceMs = s->lastScanMs = 0;
  s->scans = 0;
  s->scannedOnce = false;
}

// An STA disconnect (or boot without the hub): search fast again, scan now.
static inline void scanSchedReset(ScanSched *s, uint32_t nowMs) {
  s->searchSinceMs = nowMs;
  s->scans = 0;
  s->scannedOnce = false;
}

// The hub was in a scan result: the fast window restarts from here, so a
// join that fails right after a sighting is retried quickly.
static inline void scanSchedSighted(ScanSched *s, uint32_t nowMs) {
  s->searchSinceMs = nowMs;
}

static inline uint32_t scanSchedInterval(const ScanSched *s, uint32_t nowMs) {
  return (uint32_t)(nowMs - s->searchSinceMs) >= s->slowAfterMs ? s->slowMs
                                                                 : s->fastMs;
}

static inline bool scanSchedDue(const ScanSched *s, uint32_t nowMs) {
  if (!s->scannedOnce) return true;
  return (uint32_t)(nowMs - s->lastScanMs) >= scanSchedInterval(s, nowMs);
}

// Record a scan starting now. Returns the channel to scan: `knownChannel`,
// or 0 (all channels) when unknown or on every `fullEvery`th scan.
static inline uint8_t scanSchedStart(ScanSched *s, uint32_t nowMs,
                                     uint8_t knownChannel) {
  const bool full = !knownChannel || (s->scans % s->fullEvery) == 0;
  s->lastScanMs = nowMs;
  s->scannedOnce = true;
  s->scans++;
  return full ? 0 : knownChannel;
}

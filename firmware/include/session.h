#pragma once

// Session identity and time anchoring. Protocol v1 §3.
//
// A "session" is one boot. Every record the hub ever sees carries boot_id plus
// a relative millis() timestamp, and the hub turns that into wall time using an
// anchor this module holds.
//
// The anchor is NEVER invented. Until the hub pushes time, sessionAnchorValid()
// is false and /api/v1/session reports "anchor": null. A fabricated timestamp
// silently corrupts month-over-month baselining, which is the one failure the
// analysis layer cannot absorb.

#include <stdint.h>

// Trust ordering. A higher value never gets overwritten by a lower one.
enum SessionTimeSource : uint8_t {
  TIME_NONE = 0,
  TIME_RTC  = 1,
  TIME_NTP  = 2,
  TIME_GPS  = 3,
};

// Reads the NVS boot counter, increments it, and derives device_id from the
// MAC. Call once from setup(), before anything reports identity.
void sessionBegin();

uint32_t sessionBootId();
uint32_t sessionDeviceId();

// Wall-clock anchor. Returns false if `source` is weaker than what is already
// held -- a GPS anchor is not replaced by a later RTC one.
bool sessionSetAnchor(uint64_t epoch_ms, SessionTimeSource source);

bool               sessionAnchorValid();
uint64_t           sessionAnchorEpochMs();
uint32_t           sessionAnchorUptimeMs();
SessionTimeSource  sessionTimeSource();
const char        *sessionTimeSourceName();

// Call from loop(). millis() rolls over at ~49.7 days; on detecting the wrap
// this closes the session and starts a new boot_id so relative time stays
// monotonic within a session (protocol §3.5). Returns true on the tick it
// rolled, so callers can react.
bool sessionTick();

// Projected retention for the 1 Hz snapshot log, computed from the ID count
// actually observed at runtime rather than from a hard-coded car.
//
//   block = u32 ms + u16 count + N x 13 bytes
//
// Phase A has no filesystem yet, so this projects against the LittleFS
// partition size. It is reported on /api/v1/session and hub/health so the
// number is always measured, never assumed -- and so the BMW, which will have
// a different ID count, needs no code change.
uint32_t sessionSnapshotBytesPerSec(uint16_t idCount);
uint32_t sessionSnapshotSeconds(uint16_t idCount);

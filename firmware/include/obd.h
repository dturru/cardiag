// cardiag — OBD-II request/response layer (Phase 1a)
//
// DRAFT, 2026-08-26. Written before the board arrived — never compiled against
// hardware, never run against a vehicle. Treat every claim below as intent.
//
// Scope is deliberately narrow: standard OBD-II Mode 01 (current data) using
// SINGLE-FRAME requests and responses only. That covers the whole Tier-B signal
// set and needs no transport-protocol layer at all.
//
// What this file does NOT do, on purpose:
//   - ISO-TP multi-frame (flow control). Needed later for Mode 06 and Mode 09
//     VIN reads. A multi-frame reply is DETECTED and reported, not decoded.
//   - Any UDS service. Standard modes 01/02/03/06/07/09 are read-only by
//     design; UDS can put a module into a diagnostic session and is not worth
//     discovering at 70 mph.

#pragma once

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Bus addressing (ISO 15765-4)
// ---------------------------------------------------------------------------

// Functional (broadcast) request address. Every emissions ECU listens here,
// which is why more than one may answer a single request.
#define OBD_REQ_ID_FUNCTIONAL 0x7DF

// Physical response addresses. The ECU that answers picks one of these.
#define OBD_RESP_ID_FIRST 0x7E8
#define OBD_RESP_ID_LAST  0x7EF

#define OBD_MODE_CURRENT_DATA 0x01
#define OBD_RESPONSE_OFFSET   0x40   // reply mode = request mode + 0x40

// PIDs 0x01..0x60 are covered by three support bitmaps (0x00, 0x20, 0x40).
#define OBD_MAX_PID 96
#define OBD_BITMAP_WORDS 3

// ---------------------------------------------------------------------------

// One decoded signal definition. `decode` receives the payload bytes that
// follow the mode+PID echo, so d[0] is "A", d[1] is "B", matching how every
// published PID formula is written.
struct ObdPid {
  uint8_t     pid;
  const char *name;
  const char *unit;
  uint8_t     nbytes;               // payload bytes expected after the echo
  float (*decode)(const uint8_t *d);
};

extern const ObdPid  OBD_PID_TABLE[];
extern const size_t  OBD_PID_TABLE_LEN;

struct ObdResult {
  bool     ok;          // a well-formed reply for the requested PID arrived
  bool     multiframe;  // reply was ISO-TP multi-frame — not decoded, see above
  uint8_t  data[8];     // payload following the mode+PID echo
  uint8_t  len;
  uint32_t respId;      // which ECU answered
  uint32_t elapsedMs;
};

struct ObdStats {
  uint32_t requests;
  uint32_t replies;
  uint32_t timeouts;
  uint32_t malformed;
  uint32_t multiframe;
  uint16_t respondersMask;   // bit n set => 0x7E8+n has answered at least once
  uint32_t lastLatencyMs;
};

// Send one Mode `mode` request for `pid` and wait for the reply.
// Blocks up to OBD_RESPONSE_TIMEOUT_MS. Returns out->ok.
bool obdRequest(uint8_t mode, uint8_t pid, ObdResult *out);

// Query PIDs 0x00 / 0x20 / 0x40 and fill `bitmap` (OBD_BITMAP_WORDS words) with
// what this specific vehicle reports as supported. Returns the number of
// supported PIDs found. Nothing in this module assumes a PID exists.
uint8_t obdDiscoverSupported(uint32_t *bitmap);

// Test one PID against a bitmap filled by obdDiscoverSupported().
bool obdPidSupported(const uint32_t *bitmap, uint8_t pid);

// Table lookup; returns nullptr if the PID has no decoder here.
const ObdPid *obdFindPid(uint8_t pid);

const ObdStats *obdStats(void);
void obdResetStats(void);

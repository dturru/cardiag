#pragma once

// ---------------------------------------------------------------------------
// THE POLL PLAN: what MODE_POLL asks for, supplied by the hub.
//
// The logger does not know the car (protocol rule 3). Which PIDs to read and
// how often is the hub's decision: POST /api/v1/pollplan. This module
// validates the body exactly as carhub docs/protocol.md specifies, keeps the
// plan in canonical (sorted) form, hashes that canonical form, and schedules
// it. No plan -> MODE_POLL idles and says so.
//
// Pure, no Arduino/FreeRTOS: test/test_pollplan drives validation, the hash
// vector and the scheduler natively. poller.cpp is the CAN + NVS glue.
//
// Contract (protocol v1):
//   body   {"version":1,"entries":[{"mode":1,"pid":12,"period_ms":200}, ...]}
//   mode   1 only (Mode 0x22 is reserved for v2 with a "did" field)
//   pid    0..255        period_ms 100..60000        entries <= 32
//   duplicate (mode,pid) -> 422; empty entries = clear the plan
//   400 malformed JSON; 422 {"ok":false,"error":"<reason>"}
//   hash   SHA-256 of the canonical compact JSON (entries sorted by
//          (mode,pid), keys mode,pid,period_ms), first 16 hex chars
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#define POLL_PLAN_MAX        32
#define POLL_PERIOD_MIN_MS   100u
#define POLL_PERIOD_MAX_MS   60000u
#define POLL_JSON_MAX        4096u
#define POLL_HASH_HEX        16

struct PollEntry {
  uint8_t  mode;
  uint8_t  pid;
  uint32_t periodMs;
};

struct PollPlan {
  uint8_t   n;
  PollEntry e[POLL_PLAN_MAX];   // canonical: sorted by (mode, pid)
};

enum PollPlanStatus : uint8_t {
  POLLPLAN_OK = 0,
  POLLPLAN_MALFORMED,     // 400: not well-formed JSON (or too long to read)
  POLLPLAN_INVALID,       // 422: well-formed, fails the contract
};

struct PollPlanResult {
  PollPlanStatus status;
  const char    *error;   // 422 reason, or nullptr
};

// Parse + validate a request body. On POLLPLAN_OK *out is canonical (sorted).
PollPlanResult pollPlanParse(const char *json, size_t n, PollPlan *out);

// Canonical compact JSON. Returns its length (excluding NUL), 0 if cap is
// too small. 32 entries need under 1.5 kB.
#define POLLPLAN_CANON_MAX 1536u
size_t pollPlanCanonical(const PollPlan &p, char *buf, size_t cap);

// First 16 hex chars of SHA-256(canonical JSON), NUL-terminated.
void pollPlanHash(const PollPlan &p, char out[POLL_HASH_HEX + 1]);

// NVS blob: [u8 version=1][u8 n] then n x [u8 mode][u8 pid][u32 period LE].
#define POLLPLAN_BLOB_MAX (2 + POLL_PLAN_MAX * 6)
size_t pollPlanEncode(const PollPlan &p, uint8_t *buf, size_t cap);
// False on a short, oversized, wrong-version or contract-invalid blob: NVS is
// not trusted to still hold a valid plan.
bool   pollPlanDecode(const uint8_t *buf, size_t n, PollPlan *out);

// ---------------------------------------------------------------------------
// Scheduler. One request in flight; the most overdue entry goes next; a
// minimum gap between requests; per-entry timeout backoff.
// ---------------------------------------------------------------------------

struct PollPidStats {
  uint32_t requests;
  uint32_t replies;
  uint32_t timeouts;
  uint32_t multiframe;    // First Frame seen: counted, not decoded here
  uint32_t txFailed;      // gate shut or driver refused -- never reached the bus
  uint32_t lastLatencyMs;
  uint8_t  misses;        // consecutive timeouts; drives the backoff
};

struct PollSched {
  PollPlan     plan;
  uint32_t     due[POLL_PLAN_MAX];
  PollPidStats st[POLL_PLAN_MAX];
  int8_t       inflight;  // entry index, -1 = none
  uint32_t     sentMs;
  uint32_t     lastTxMs;
  bool         anyTx;
  bool         anyOk;     // a reply has arrived since init
};

void pollSchedInit(PollSched &s, const PollPlan &p, uint32_t now);

// Entry to send now, or -1: something in flight, nothing due, or the gap
// since the last transmit has not elapsed.
int pollSchedPick(const PollSched &s, uint32_t now, uint32_t gapMs);

void pollSchedSent(PollSched &s, int i, uint32_t now);
// The frame never reached the bus (TX gate shut, driver busy). Not a timeout
// and no backoff: try again one period later.
void pollSchedTxFailed(PollSched &s, int i, uint32_t now);

enum PollRx : uint8_t { POLL_RX_IGNORED = 0, POLL_RX_OK, POLL_RX_MULTIFRAME };

// Offer one frame received from 0x7E8..0x7EF. Matches only the request in
// flight (reply mode = mode + 0x40, pid echo). On POLL_RX_OK the payload after
// the echo is copied to payload/len (len <= 5 for a single frame).
PollRx pollSchedOnFrame(PollSched &s, const uint8_t *data, uint8_t dlc,
                        uint32_t now, uint8_t *payload, uint8_t *len);

// Retire the request in flight once it has waited timeoutMs. Returns true when
// it timed out. The entry backs off: period << misses, capped at
// max(backoffMaxMs, period).
bool pollSchedCheckTimeout(PollSched &s, uint32_t now, uint32_t timeoutMs,
                           uint32_t backoffMaxMs);

uint32_t pollBackoffMs(uint32_t periodMs, uint8_t misses, uint32_t maxMs);

#pragma once

// ---------------------------------------------------------------------------
// SELFTEST bench workload -- Civic-shaped synthetic traffic.
//
// *** THIS IS A TEST FIXTURE, NOT CAR KNOWLEDGE. ***
//
// Protocol rule 3 says the logger is car-agnostic: IDs, PIDs, scaling and
// alert limits live in hub config and never in firmware. This table looks like
// a violation of that and is not one, for reasons that must stay true:
//
//   * NOTHING here is used by any RECEIVE path. No decode, no scaling, no
//     meaning is attached to any id or byte. The receive side still has no
//     idea what 0x091 is, which is the property rule 3 protects.
//   * It is only ever TRANSMITTED, and only in MODE_SELFTEST, which runs in
//     TWAI_MODE_NO_ACK internal loopback on a bench, unplugged from any car.
//   * It is a WORKLOAD SHAPE -- how many ids, how often, how much of the
//     payload moves -- not a signal map.
//
// If anything in this file is ever read by a decoding path, that is the bug.
//
// ---------------------------------------------------------------------------
// WHERE THE NUMBERS COME FROM
//
// analysis/captures_2026-09-08_civic_stimulus1.csv, the 2012 Civic stimulus
// run: 33,911 rows, 14 distinct ids, 143.2 s. Each period below is
// span / count for that id, so the synthetic run reproduces the MEASURED rate
// per id and the ~237 rows/s aggregate rather than a guess.
//
// 🔑 THAT CAPTURE IS THE CHANGE LOG, NOT THE RAW BUS. These are CHANGE rates.
// The raw Civic bus runs closer to 1000 fps; most of it is identical repeats
// that the change log drops. So this profile deliberately drives the hub path
// at the rate at which state actually MOVES, which is what the snapshot
// stream, the changed-mask and the fast list all key on. It is NOT a bus-load
// test, and a 6% loopback bus load is not evidence about a real one.
//
// The old workload was ONE id at 1 Hz. Under that, a snapshot packet never
// carried more than one record, so multi-id packets and the fast list were
// never exercised at all -- both were listed UNVERIFIED for exactly that
// reason.
//
// ⚠ What this still does NOT exercise: the >64-record packet split in
// hubstream's sendRows(). That is unreachable by construction, not untested --
// SNIFF_MAX_IDS (64) equals HUB_MAX_RECORDS (64), so a snapshot can never
// produce a 65th row. The loop is there for a larger table, and testing it
// needs SNIFF_MAX_IDS raised, not more traffic.
// ---------------------------------------------------------------------------

#include <stdint.h>

struct SelfTestId {
  uint16_t id;
  uint8_t  dlc;
  uint16_t periodMs;   // span / count from the capture
};

// Ordered by rate, fastest first. 0x378 appeared exactly once in 143 s, so it
// has no meaningful period: it is emitted once per run and then left alone,
// which also gives the snapshot a genuinely stale id to carry.
static const SelfTestId SELFTEST_IDS[] = {
  { 0x091, 8,   20 },   // n=7285  50.9 Hz
  { 0x156, 6,   23 },   // n=6292  44.0 Hz
  { 0x158, 8,   26 },   // n=5477  38.3 Hz
  { 0x13C, 8,   36 },   // n=3974  27.8 Hz
  { 0x17C, 8,   47 },   // n=3042  21.3 Hz
  { 0x1DC, 4,   47 },   // n=3039  21.2 Hz
  { 0x1A6, 8,   53 },   // n=2696  18.8 Hz
  { 0x18E, 3,  167 },   // n=859    6.0 Hz
  { 0x40C, 8,  300 },   // n=477    3.3 Hz
  { 0x324, 8,  308 },   // n=465    3.3 Hz
  { 0x1A4, 8,  961 },   // n=149    1.0 Hz
  { 0x1AA, 8,  961 },   // n=149    1.0 Hz
  { 0x294, 8, 23867 },  // n=6      0.04 Hz
  { 0x378, 8,     0 },  // n=1      once, then never again
};

static const uint16_t SELFTEST_ID_COUNT =
    (uint16_t)(sizeof(SELFTEST_IDS) / sizeof(SELFTEST_IDS[0]));

// The id put on the fast list by default when SELFTEST starts. 0x091 is the
// fastest id in the capture, so it is the one whose rate is measurable against
// HUB_FAST_HZ without being limited by how often the id itself updates.
#define SELFTEST_FAST_ID 0x091

// How often the non-heartbeat byte steps. Slow enough to survive the
// heartbeat filter (SNIFF_HEARTBEAT_PCT), which is the point: without a byte
// that moves rarely, every byte looks like a counter and the change log has
// nothing to record. See the payload comment in main.cpp.
#define SELFTEST_SIGNAL_STEP_MS 500

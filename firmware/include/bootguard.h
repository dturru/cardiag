#pragma once

// ---------------------------------------------------------------------------
// Boot-attempt counter for the filestore start, and the SAFE MODE decision.
//
// 🐛 WHY (2026-09-25). After a 200-cycle soak the board spent 187 s inside
// filestoreBegin() -- a quadratic directory scan -- with the task watchdog not
// yet armed, because it was deliberately armed only AFTER filestoreBegin(). A
// slower filesystem, or a real hang, would have held the board there forever,
// awake on an unswitched battery feed with no radio up to say so.
//
// The fix has two halves:
//   * an EARLY watchdog, armed before filestoreBegin() with a timeout derived
//     from the measured worst-case scan (config.h, FS_BOOT_WDT_S), and
//   * this counter, so a boot that the early watchdog kills is REMEMBERED.
//     Two consecutive filestore starts that never finished => SAFE MODE: skip
//     the filestore, bring up Wi-Fi and the API, report `filestore_failed`,
//     and wait for a person (or the hub) to decide. Never an automatic format.
//
// Where the count lives:
//   RTC_NOINIT memory  survives a watchdog/panic reset, NOT a power loss
//   NVS                survives both -- the fallback, because on the carrier
//                      INH can remove power at any time
// Both are written; the higher valid count wins, since either can lag.
//
// A record is only valid for the firmware that wrote it (`appTag`, from the
// app ELF sha). Flashing a fix therefore starts from zero automatically -- a
// board stuck in safe mode on a bad build must not stay there after a good
// one is flashed.
//
// This header is the DECISION only, no I/O, so `pio test -e native` covers it.
// The RTC/NVS/watchdog plumbing is in main.cpp.
// ---------------------------------------------------------------------------

#include <stdint.h>

#define BG_MAGIC 0xB0075AFEu

// Consecutive unfinished filestore starts before safe mode. "2+ consecutive
// failed attempts" -- one can be a brownout mid-scan; two is a pattern.
#ifndef BG_FAIL_LIMIT
#define BG_FAIL_LIMIT 2
#endif

// RTC_NOINIT memory holds garbage after a power-on, so a record needs a check
// word as well as the magic. Any 32-bit mix that is not the identity will do;
// this is corruption detection, not security.
static inline uint32_t bgCheck(uint32_t appTag, uint8_t attempts) {
  uint32_t x = BG_MAGIC ^ appTag ^ ((uint32_t)attempts * 0x9E3779B1u);
  x ^= x >> 16;
  return x;
}

struct BgRecord {
  uint32_t magic;
  uint32_t appTag;
  uint32_t check;
  uint8_t  attempts;
};

static inline bool bgValid(const BgRecord *r, uint32_t appTag) {
  return r->magic == BG_MAGIC && r->appTag == appTag &&
         r->check == bgCheck(r->appTag, r->attempts);
}

static inline BgRecord bgMake(uint32_t appTag, uint8_t attempts) {
  BgRecord r;
  r.magic = BG_MAGIC;
  r.appTag = appTag;
  r.attempts = attempts;
  r.check = bgCheck(appTag, attempts);
  return r;
}

// Unfinished attempts before this boot. An invalid or other-firmware record
// counts as zero.
static inline uint8_t bgPriorAttempts(const BgRecord *rtc, const BgRecord *nvs,
                                      uint32_t appTag) {
  const uint8_t a = (rtc && bgValid(rtc, appTag)) ? rtc->attempts : 0;
  const uint8_t b = (nvs && bgValid(nvs, appTag)) ? nvs->attempts : 0;
  return a > b ? a : b;
}

static inline bool bgShouldSafeMode(uint8_t prior) {
  return prior >= BG_FAIL_LIMIT;
}

// The count to write BEFORE starting the filestore. Saturates; never wraps
// back to "healthy".
static inline uint8_t bgOnStart(uint8_t prior) {
  return prior == 0xFF ? 0xFF : (uint8_t)(prior + 1);
}

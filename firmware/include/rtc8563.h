#pragma once

// ---------------------------------------------------------------------------
// PCF8563 RTC: register decode/encode and the validity rule, as pure
// functions. The I2C glue is rtc.cpp; this half is what test/test_rtc8563
// drives natively.
//
// THE RULE: the RTC is the LOWEST-trust anchor (session.h: rtc < ntp < gps),
// and it is used ONLY when its voltage-low (VL) flag is clear. VL set means the
// oscillator stopped at some point (coin cell flat or removed) and the time is
// not guaranteed -- so there is NO anchor, not a guessed one. A fabricated
// timestamp silently corrupts month-over-month baselining (session.h).
//
// Registers 0x02..0x08, read as one 7-byte burst:
//   [0] VL_seconds   bit7 = VL, bits 6..0 = seconds (BCD)
//   [1] minutes      bits 6..0 (BCD)
//   [2] hours        bits 5..0 (BCD, 24 h)
//   [3] days         bits 5..0 (BCD, 1..31)
//   [4] weekdays     bits 2..0 (0..6) -- ignored on read, derived on write
//   [5] century_mon  bit7 = C, bits 4..0 = month (BCD, 1..12)
//   [6] years        BCD 00..99
//
// Century: this firmware writes C = 0 for 20xx and REJECTS C = 1 on read.
// The datasheet leaves the meaning to the user; refusing the bit we never
// write is the never-invent-time reading.
// ---------------------------------------------------------------------------

#include <stdint.h>

#define PCF8563_I2C_ADDR   0x51
#define PCF8563_REG_TIME   0x02
#define PCF8563_TIME_LEN   7

enum RtcStatus : uint8_t {
  RTC_OK = 0,
  RTC_VOLTAGE_LOW,     // VL set: time not guaranteed -- no anchor
  RTC_BAD_FIELD,       // not BCD, or out of range (bus error, garbage)
  RTC_BAD_CENTURY,     // C = 1: never written by this firmware
  RTC_TOO_EARLY,       // before minYear: an unset clock counting from reset
};

static inline const char *rtcStatusName(RtcStatus s) {
  switch (s) {
    case RTC_OK:          return "ok";
    case RTC_VOLTAGE_LOW: return "voltage-low flag set";
    case RTC_BAD_FIELD:   return "invalid register value";
    case RTC_BAD_CENTURY: return "century bit set";
    case RTC_TOO_EARLY:   return "before the plausible minimum year";
  }
  return "?";
}

// BCD -> binary; -1 if either nibble is not a decimal digit.
static inline int rtcBcd(uint8_t b) {
  const int hi = b >> 4, lo = b & 0x0F;
  return (hi > 9 || lo > 9) ? -1 : hi * 10 + lo;
}

static inline uint8_t rtcToBcd(int v) {
  return (uint8_t)(((v / 10) << 4) | (v % 10));
}

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant).
static inline int64_t rtcDaysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static inline void rtcCivilFromDays(int64_t z, int *y, unsigned *m,
                                    unsigned *d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = (int)(yoe + era * 400) + (*m <= 2);
}

static inline unsigned rtcDaysInMonth(int y, unsigned m) {
  static const uint8_t k[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return (m == 2 && leap) ? 29 : k[m - 1];
}

// Decode a 7-byte burst. On RTC_OK, *epochS is UTC seconds since 1970.
// VL is checked FIRST: a voltage-low clock is rejected even if its fields
// happen to look plausible, which is exactly the case that would fool a
// range check alone.
static inline RtcStatus rtcDecode(const uint8_t r[PCF8563_TIME_LEN],
                                  int minYear, int64_t *epochS) {
  if (r[0] & 0x80) return RTC_VOLTAGE_LOW;
  if (r[5] & 0x80) return RTC_BAD_CENTURY;
  const int s  = rtcBcd(r[0] & 0x7F);
  const int mi = rtcBcd(r[1] & 0x7F);
  const int h  = rtcBcd(r[2] & 0x3F);
  const int d  = rtcBcd(r[3] & 0x3F);
  const int mo = rtcBcd(r[5] & 0x1F);
  const int yy = rtcBcd(r[6]);
  if (s < 0 || mi < 0 || h < 0 || d < 0 || mo < 0 || yy < 0) return RTC_BAD_FIELD;
  if (s > 59 || mi > 59 || h > 23 || mo < 1 || mo > 12 || d < 1) return RTC_BAD_FIELD;
  const int y = 2000 + yy;
  if (d > (int)rtcDaysInMonth(y, (unsigned)mo)) return RTC_BAD_FIELD;
  if (y < minYear) return RTC_TOO_EARLY;
  *epochS = rtcDaysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400 +
            h * 3600 + mi * 60 + s;
  return RTC_OK;
}

// Encode UTC seconds into a 7-byte burst for register 0x02. Writing the
// seconds register with bit7 = 0 is what clears VL. False outside 2000..2099
// (the range C = 0 can represent).
static inline bool rtcEncode(int64_t epochS, uint8_t r[PCF8563_TIME_LEN]) {
  if (epochS < 0) return false;
  const int64_t days = epochS / 86400;
  const int64_t sod = epochS % 86400;
  int y;
  unsigned m, d;
  rtcCivilFromDays(days, &y, &m, &d);
  if (y < 2000 || y > 2099) return false;
  r[0] = rtcToBcd((int)(sod % 60));            // VL = 0
  r[1] = rtcToBcd((int)(sod / 60 % 60));
  r[2] = rtcToBcd((int)(sod / 3600));
  r[3] = rtcToBcd((int)d);
  r[4] = (uint8_t)((days + 4) % 7);            // 1970-01-01 was a Thursday
  r[5] = rtcToBcd((int)m);                     // C = 0
  r[6] = rtcToBcd(y - 2000);
  return true;
}

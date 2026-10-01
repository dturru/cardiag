// rtc8563.h: PCF8563 register decode/encode and the never-invent-time rule.

#include <string.h>
#include <unity.h>

#include "rtc8563.h"

void setUp(void) {}
void tearDown(void) {}

static const int MIN_YEAR = 2026;

// 2026-10-01 12:34:56 UTC = 1790858096 (date -u -d @1790858096).
static const uint8_t kGood[7] = {0x56, 0x34, 0x12, 0x01, 0x04, 0x10, 0x26};
static const int64_t kGoodEpoch = 1790858096;

void test_decode_valid_time(void) {
  int64_t s = 0;
  TEST_ASSERT_EQUAL(RTC_OK, rtcDecode(kGood, MIN_YEAR, &s));
  TEST_ASSERT_EQUAL_INT64(kGoodEpoch, s);
}

// ⭐ VL set: rejected even though every field is plausible.
void test_voltage_low_is_never_used(void) {
  uint8_t r[7];
  memcpy(r, kGood, 7);
  r[0] |= 0x80;
  int64_t s = -1;
  TEST_ASSERT_EQUAL(RTC_VOLTAGE_LOW, rtcDecode(r, MIN_YEAR, &s));
  TEST_ASSERT_EQUAL_INT64(-1, s);                 // output untouched
}

void test_century_bit_rejected(void) {
  uint8_t r[7];
  memcpy(r, kGood, 7);
  r[5] |= 0x80;
  int64_t s;
  TEST_ASSERT_EQUAL(RTC_BAD_CENTURY, rtcDecode(r, MIN_YEAR, &s));
}

void test_bad_fields_rejected(void) {
  int64_t s;
  uint8_t r[7];
  const struct { int idx; uint8_t v; } bad[] = {
    {0, 0x60}, {0, 0x5A}, {1, 0x60}, {2, 0x24}, {3, 0x00}, {3, 0x32},
    {5, 0x00}, {5, 0x13}, {6, 0x9A},
  };
  for (const auto &b : bad) {
    memcpy(r, kGood, 7);
    r[b.idx] = b.v;
    TEST_ASSERT_EQUAL_MESSAGE(RTC_BAD_FIELD, rtcDecode(r, MIN_YEAR, &s), "field");
  }
  // 31 September does not exist; 29 Feb only in a leap year.
  memcpy(r, kGood, 7); r[3] = 0x31; r[5] = 0x09;
  TEST_ASSERT_EQUAL(RTC_BAD_FIELD, rtcDecode(r, MIN_YEAR, &s));
  memcpy(r, kGood, 7); r[3] = 0x29; r[5] = 0x02; r[6] = 0x27;
  TEST_ASSERT_EQUAL(RTC_BAD_FIELD, rtcDecode(r, MIN_YEAR, &s));
  r[6] = 0x28;
  TEST_ASSERT_EQUAL(RTC_OK, rtcDecode(r, MIN_YEAR, &s));
}

// A clock reset to its power-on value (2000-01-01) with VL somehow clear is
// still not a time.
void test_before_min_year_rejected(void) {
  const uint8_t r[7] = {0x00, 0x00, 0x00, 0x01, 0x06, 0x01, 0x00};
  int64_t s;
  TEST_ASSERT_EQUAL(RTC_TOO_EARLY, rtcDecode(r, MIN_YEAR, &s));
}

void test_encode_clears_vl_and_round_trips(void) {
  uint8_t r[7];
  TEST_ASSERT_TRUE(rtcEncode(kGoodEpoch, r));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(kGood, r, 7);      // incl. weekday 4 (Thu)
  TEST_ASSERT_EQUAL_HEX8(0, r[0] & 0x80);
  // Round trip across leap days and year ends.
  const int64_t pts[] = {1772323199, 1772323200, 1830297599, 1830297600,
                         4102444799};             // ... 2099-12-31 23:59:59
  for (int64_t e : pts) {
    int64_t back = 0;
    TEST_ASSERT_TRUE(rtcEncode(e, r));
    TEST_ASSERT_EQUAL(RTC_OK, rtcDecode(r, MIN_YEAR, &back));
    TEST_ASSERT_EQUAL_INT64(e, back);
  }
}

void test_encode_out_of_range_refused(void) {
  uint8_t r[7];
  TEST_ASSERT_FALSE(rtcEncode(-1, r));
  TEST_ASSERT_FALSE(rtcEncode(946684799, r));     // 1999-12-31 23:59:59
  TEST_ASSERT_FALSE(rtcEncode(4102444800, r));    // 2100-01-01
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_decode_valid_time);
  RUN_TEST(test_voltage_low_is_never_used);
  RUN_TEST(test_century_bit_rejected);
  RUN_TEST(test_bad_fields_rejected);
  RUN_TEST(test_before_min_year_rejected);
  RUN_TEST(test_encode_clears_vl_and_round_trips);
  RUN_TEST(test_encode_out_of_range_refused);
  return UNITY_END();
}

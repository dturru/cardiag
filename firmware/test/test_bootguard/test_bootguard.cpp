// The safe-mode decision, tested on the host.
//
// The failure this guards against: a filestore start that never finishes
// (the 187 s scan after the 200-cycle soak) must not be retried forever. Two
// unfinished starts in a row => safe mode. Everything below pins what counts
// as "in a row" across the two places the count lives.

#include <unity.h>

#include "bootguard.h"

static const uint32_t APP  = 0x12345678u;
static const uint32_t APP2 = 0x9ABCDEF0u;

void test_no_records_is_a_clean_boot(void) {
  TEST_ASSERT_EQUAL_UINT8(0, bgPriorAttempts(nullptr, nullptr, APP));
  TEST_ASSERT_FALSE(bgShouldSafeMode(0));
}

void test_power_on_garbage_in_rtc_is_ignored(void) {
  // RTC_NOINIT holds noise after a power-on. It must not read as a count.
  BgRecord junk = {0xDEADBEEFu, APP, 0x1234u, 200};
  TEST_ASSERT_EQUAL_UINT8(0, bgPriorAttempts(&junk, nullptr, APP));
  // Right magic, wrong check word: still noise.
  BgRecord bad = bgMake(APP, 5);
  bad.check ^= 1;
  TEST_ASSERT_EQUAL_UINT8(0, bgPriorAttempts(&bad, nullptr, APP));
}

void test_one_unfinished_start_is_not_yet_safe_mode(void) {
  // One can be a brownout mid-scan.
  BgRecord r = bgMake(APP, 1);
  const uint8_t prior = bgPriorAttempts(&r, &r, APP);
  TEST_ASSERT_EQUAL_UINT8(1, prior);
  TEST_ASSERT_FALSE(bgShouldSafeMode(prior));
}

void test_two_unfinished_starts_enter_safe_mode(void) {
  BgRecord r = bgMake(APP, 2);
  TEST_ASSERT_TRUE(bgShouldSafeMode(bgPriorAttempts(&r, &r, APP)));
}

void test_nvs_carries_the_count_across_a_power_loss(void) {
  // INH cut the power: RTC is gone, NVS is not.
  BgRecord nvs = bgMake(APP, 2);
  TEST_ASSERT_TRUE(bgShouldSafeMode(bgPriorAttempts(nullptr, &nvs, APP)));
}

void test_rtc_carries_the_count_when_the_nvs_write_lagged(void) {
  // The watchdog fired between the RTC write and the NVS commit.
  BgRecord rtc = bgMake(APP, 2), nvs = bgMake(APP, 1);
  TEST_ASSERT_EQUAL_UINT8(2, bgPriorAttempts(&rtc, &nvs, APP));
}

void test_the_higher_valid_count_wins_either_way(void) {
  BgRecord rtc = bgMake(APP, 1), nvs = bgMake(APP, 3);
  TEST_ASSERT_EQUAL_UINT8(3, bgPriorAttempts(&rtc, &nvs, APP));
}

void test_flashing_new_firmware_starts_from_zero(void) {
  // A board stuck in safe mode on a bad build must not stay there once a
  // fixed build is flashed.
  BgRecord old = bgMake(APP, 7);
  TEST_ASSERT_EQUAL_UINT8(0, bgPriorAttempts(&old, &old, APP2));
}

void test_a_cleared_record_is_a_clean_boot(void) {
  // Success writes attempts = 0, not "no record".
  BgRecord r = bgMake(APP, 0);
  TEST_ASSERT_EQUAL_UINT8(0, bgPriorAttempts(&r, &r, APP));
}

void test_start_increments_and_saturates(void) {
  TEST_ASSERT_EQUAL_UINT8(1, bgOnStart(0));
  TEST_ASSERT_EQUAL_UINT8(2, bgOnStart(1));
  // Never wraps back to zero, which would read as healthy.
  TEST_ASSERT_EQUAL_UINT8(0xFF, bgOnStart(0xFE));
  TEST_ASSERT_EQUAL_UINT8(0xFF, bgOnStart(0xFF));
}

void test_sequence_hang_hang_is_safe_mode_on_the_third_boot(void) {
  // Boot 1: start (1), watchdog. Boot 2: start (2), watchdog. Boot 3: safe.
  uint8_t prior = bgPriorAttempts(nullptr, nullptr, APP);
  TEST_ASSERT_FALSE(bgShouldSafeMode(prior));
  BgRecord r = bgMake(APP, bgOnStart(prior));           // boot 1 dies here

  prior = bgPriorAttempts(&r, &r, APP);
  TEST_ASSERT_FALSE(bgShouldSafeMode(prior));
  r = bgMake(APP, bgOnStart(prior));                     // boot 2 dies here

  prior = bgPriorAttempts(&r, &r, APP);
  TEST_ASSERT_TRUE(bgShouldSafeMode(prior));             // boot 3
}

void test_sequence_hang_ok_hang_never_reaches_safe_mode(void) {
  // "Consecutive" means consecutive: a success in between clears it.
  BgRecord r = bgMake(APP, bgOnStart(0));                // boot 1 dies
  uint8_t prior = bgPriorAttempts(&r, &r, APP);
  r = bgMake(APP, bgOnStart(prior));                     // boot 2 starts...
  r = bgMake(APP, 0);                                    // ...and finishes
  prior = bgPriorAttempts(&r, &r, APP);
  r = bgMake(APP, bgOnStart(prior));                     // boot 3 dies
  prior = bgPriorAttempts(&r, &r, APP);
  TEST_ASSERT_EQUAL_UINT8(1, prior);
  TEST_ASSERT_FALSE(bgShouldSafeMode(prior));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_no_records_is_a_clean_boot);
  RUN_TEST(test_power_on_garbage_in_rtc_is_ignored);
  RUN_TEST(test_one_unfinished_start_is_not_yet_safe_mode);
  RUN_TEST(test_two_unfinished_starts_enter_safe_mode);
  RUN_TEST(test_nvs_carries_the_count_across_a_power_loss);
  RUN_TEST(test_rtc_carries_the_count_when_the_nvs_write_lagged);
  RUN_TEST(test_the_higher_valid_count_wins_either_way);
  RUN_TEST(test_flashing_new_firmware_starts_from_zero);
  RUN_TEST(test_a_cleared_record_is_a_clean_boot);
  RUN_TEST(test_start_increments_and_saturates);
  RUN_TEST(test_sequence_hang_hang_is_safe_mode_on_the_third_boot);
  RUN_TEST(test_sequence_hang_ok_hang_never_reaches_safe_mode);
  return UNITY_END();
}

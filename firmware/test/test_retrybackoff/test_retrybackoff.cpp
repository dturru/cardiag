// backoffNextMs() / backoffReset(): the hub rejoin schedule, host-tested.
//
// 5, 10, 20, 40, then 60 s capped; restarts on every STA disconnect.

#include <unity.h>

#include "retrybackoff.h"

static RetryBackoff b;

static void fresh(void) { backoffInit(&b, 5000, 60000); }

void setUp(void) {}
void tearDown(void) {}

void test_schedule_is_5_10_20_40_then_60_capped(void) {
  fresh();
  const uint32_t want[] = {5000, 10000, 20000, 40000, 60000, 60000, 60000};
  for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
    TEST_ASSERT_EQUAL_UINT32(want[i], backoffNextMs(&b));
  }
}

void test_a_disconnect_restarts_the_schedule(void) {
  fresh();
  for (int i = 0; i < 6; i++) backoffNextMs(&b);          // settled at 60 s
  backoffReset(&b);
  TEST_ASSERT_EQUAL_UINT32(5000, backoffNextMs(&b));
  TEST_ASSERT_EQUAL_UINT32(10000, backoffNextMs(&b));
}

void test_reset_mid_schedule(void) {
  fresh();
  backoffNextMs(&b);                                     // 5
  backoffNextMs(&b);                                     // 10
  backoffReset(&b);
  TEST_ASSERT_EQUAL_UINT32(5000, backoffNextMs(&b));
}

void test_never_exceeds_the_cap_or_wraps_after_many_failures(void) {
  fresh();
  // A hub absent for days: hundreds of failed joins. The shift must not
  // overflow into a short wait, and step must not wrap back to 5 s.
  for (int i = 0; i < 1000; i++) {
    const uint32_t d = backoffNextMs(&b);
    TEST_ASSERT_TRUE(d >= 5000 && d <= 60000);
    if (i >= 4) TEST_ASSERT_EQUAL_UINT32(60000, d);
  }
}

void test_a_first_delay_above_the_cap_is_clamped(void) {
  fresh();
  backoffInit(&b, 90000, 60000);
  TEST_ASSERT_EQUAL_UINT32(60000, backoffNextMs(&b));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_schedule_is_5_10_20_40_then_60_capped);
  RUN_TEST(test_a_disconnect_restarts_the_schedule);
  RUN_TEST(test_reset_mid_schedule);
  RUN_TEST(test_never_exceeds_the_cap_or_wraps_after_many_failures);
  RUN_TEST(test_a_first_delay_above_the_cap_is_clamped);
  return UNITY_END();
}

// busIdleFor() / busQuietMs(): the clean key-off test, host-tested.
//
// The first two cases are the soak failure on #6 (2026-09-25): canTask stored
// a bus timestamp newer than the `now` loop() had cached, and the unsigned
// difference read as ~49.7 days of silence.

#include <unity.h>

#include "busidle.h"

static const uint32_t T = 3000;   // CAN_BUS_IDLE_CLOSE_MS

void test_activity_newer_than_cached_now_is_not_idle(void) {
  const uint32_t now = 100000;
  for (uint32_t ahead = 1; ahead <= 5000; ahead += 7) {
    TEST_ASSERT_FALSE(busIdleFor(now, now + ahead, T));
    TEST_ASSERT_EQUAL_UINT32(0, busQuietMs(now, now + ahead));
  }
}

void test_the_old_unsigned_arithmetic_would_have_said_idle(void) {
  // Pins what the bug was, so the test above is known to cover it.
  const uint32_t now = 100000, last = now + 1;
  TEST_ASSERT_TRUE((uint32_t)(now - last) >= T);
  TEST_ASSERT_FALSE(busIdleFor(now, last, T));
}

void test_quiet_for_the_threshold_is_idle(void) {
  TEST_ASSERT_FALSE(busIdleFor(10000, 10000 - (T - 1), T));
  TEST_ASSERT_TRUE(busIdleFor(10000, 10000 - T, T));
  TEST_ASSERT_EQUAL_UINT32(T, busQuietMs(10000, 10000 - T));
}

void test_never_seen_a_frame_is_never_idle(void) {
  TEST_ASSERT_FALSE(busIdleFor(0xFFFFFFF0u, 0, T));
  TEST_ASSERT_EQUAL_UINT32(0, busQuietMs(123456, 0));
}

void test_across_the_millis_rollover(void) {
  // last just before the 32-bit wrap, now just after it.
  const uint32_t last = 0xFFFFFF00u;
  TEST_ASSERT_FALSE(busIdleFor(last + 100, last, T));        // 100 ms
  TEST_ASSERT_TRUE(busIdleFor(last + T + 5, last, T));       // wraps past 0
  TEST_ASSERT_EQUAL_UINT32(T + 5, busQuietMs(last + T + 5, last));
  // And a "future" timestamp across the wrap is still not idle.
  TEST_ASSERT_FALSE(busIdleFor(last, last + 300, T));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_activity_newer_than_cached_now_is_not_idle);
  RUN_TEST(test_the_old_unsigned_arithmetic_would_have_said_idle);
  RUN_TEST(test_quiet_for_the_threshold_is_idle);
  RUN_TEST(test_never_seen_a_frame_is_never_idle);
  RUN_TEST(test_across_the_millis_rollover);
  return UNITY_END();
}

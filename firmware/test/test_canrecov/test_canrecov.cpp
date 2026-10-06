// canrecov.h: bring a stuck TWAI transmitter back (bus-off, or error-passive
// with TX failing), with backoff, never acting faster than the backoff allows.

#include <unity.h>

#include "canrecov.h"

static CanRecov r;

void setUp(void) { canRecovInit(&r, 0); }
void tearDown(void) {}

void test_healthy_bus_does_nothing(void) {
  for (uint32_t t = 0; t < 120000; t += 1000)
    TEST_ASSERT_EQUAL(CANRECOV_NONE, canRecovStep(&r, CANBUS_RUNNING, 0, 0, t));
  TEST_ASSERT_EQUAL_UINT32(0, r.recoveries);
}

void test_bus_off_recovers_then_restarts_when_stopped(void) {
  TEST_ASSERT_EQUAL(CANRECOV_RECOVER, canRecovStep(&r, CANBUS_BUS_OFF, 128, 50, 1000));
  TEST_ASSERT_EQUAL(CANRECOV_NONE, canRecovStep(&r, CANBUS_RECOVERING, 128, 90, 2000));
  TEST_ASSERT_EQUAL(CANRECOV_RESTART, canRecovStep(&r, CANBUS_STOPPED, 0, 90, 3000));
  TEST_ASSERT_EQUAL_UINT32(2, r.recoveries);
}

void test_recovery_that_never_finishes_restarts_after_timeout(void) {
  canRecovStep(&r, CANBUS_BUS_OFF, 128, 0, 1000);
  TEST_ASSERT_EQUAL(CANRECOV_NONE,
                    canRecovStep(&r, CANBUS_RECOVERING, 128, 0, 1000 + CANRECOV_RECOVER_TIMEOUT_MS - 1));
  TEST_ASSERT_EQUAL(CANRECOV_RESTART,
                    canRecovStep(&r, CANBUS_RECOVERING, 128, 0, 1000 + CANRECOV_RECOVER_TIMEOUT_MS));
}

void test_error_passive_with_failing_tx_restarts_after_window(void) {
  uint32_t fails = 0;
  CanRecovAction a = CANRECOV_NONE;
  uint32_t t = 10000;
  for (; t < 10000 + CANRECOV_PASSIVE_MS; t += 1000) {
    fails += 235;
    a = canRecovStep(&r, CANBUS_RUNNING, 128, fails, t);
    TEST_ASSERT_EQUAL(CANRECOV_NONE, a);
  }
  fails += 235;
  TEST_ASSERT_EQUAL(CANRECOV_RESTART, canRecovStep(&r, CANBUS_RUNNING, 128, fails, t));
}

void test_error_passive_without_failures_is_left_alone(void) {
  for (uint32_t t = 0; t < 60000; t += 1000)
    TEST_ASSERT_EQUAL(CANRECOV_NONE, canRecovStep(&r, CANBUS_RUNNING, 130, 7, t));
}

void test_failures_below_passive_are_left_alone(void) {
  uint32_t fails = 0;
  for (uint32_t t = 0; t < 60000; t += 1000) {
    fails += 3;  // a full TX queue now and then, not a stuck controller
    TEST_ASSERT_EQUAL(CANRECOV_NONE, canRecovStep(&r, CANBUS_RUNNING, 40, fails, t));
  }
}

void test_backoff_spaces_repeated_bus_off(void) {
  // Recover, restart, and immediately bus-off again: the next RECOVER waits.
  canRecovStep(&r, CANBUS_BUS_OFF, 128, 0, 0);               // RECOVER, backoff 1s->2s
  canRecovStep(&r, CANBUS_STOPPED, 0, 0, 100);               // RESTART at 100, next >= 2100
  TEST_ASSERT_EQUAL(CANRECOV_NONE, canRecovStep(&r, CANBUS_BUS_OFF, 128, 0, 1000));
  TEST_ASSERT_EQUAL(CANRECOV_NONE, canRecovStep(&r, CANBUS_BUS_OFF, 128, 0, 2000));
  TEST_ASSERT_EQUAL(CANRECOV_RECOVER, canRecovStep(&r, CANBUS_BUS_OFF, 128, 0, 2100));
}

void test_backoff_caps_and_resets_after_stable_running(void) {
  uint32_t t = 0;
  for (int i = 0; i < 20; i++) {
    t = r.nextAllowedMs;
    canRecovStep(&r, CANBUS_BUS_OFF, 128, 0, t);
    canRecovStep(&r, CANBUS_STOPPED, 0, 0, t + 10);
  }
  TEST_ASSERT_EQUAL_UINT32(CANRECOV_BACKOFF_MAX_MS, r.backoffMs);
  t += 20;
  for (uint32_t s = 0; s <= CANRECOV_STABLE_MS; s += 1000)
    canRecovStep(&r, CANBUS_RUNNING, 0, 0, t + s);
  TEST_ASSERT_EQUAL_UINT32(CANRECOV_BACKOFF_MIN_MS, r.backoffMs);
}

void test_state_names(void) {
  TEST_ASSERT_EQUAL_STRING("bus_off", canBusStateName(CANBUS_BUS_OFF));
  TEST_ASSERT_EQUAL_STRING("running", canBusStateName(CANBUS_RUNNING));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_healthy_bus_does_nothing);
  RUN_TEST(test_bus_off_recovers_then_restarts_when_stopped);
  RUN_TEST(test_recovery_that_never_finishes_restarts_after_timeout);
  RUN_TEST(test_error_passive_with_failing_tx_restarts_after_window);
  RUN_TEST(test_error_passive_without_failures_is_left_alone);
  RUN_TEST(test_failures_below_passive_are_left_alone);
  RUN_TEST(test_backoff_spaces_repeated_bus_off);
  RUN_TEST(test_backoff_caps_and_resets_after_stable_running);
  RUN_TEST(test_state_names);
  return UNITY_END();
}

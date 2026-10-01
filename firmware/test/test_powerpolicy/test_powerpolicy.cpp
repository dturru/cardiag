// powerpolicy.h: policy selection, the ignition debounce, and how both feed
// the sleep guard (sleepguard.h).

#include <unity.h>

#include "powerpolicy.h"
#include "sleepguard.h"

void setUp(void) {}
void tearDown(void) {}

static const uint32_t ON_MS = 200, OFF_MS = 2000;
static const uint32_t BUS_CLOSE = 3000, BUS_SLEEP = 300000, IGN_SLEEP = 60000;

static IgnDebounce booted(bool raw, uint32_t now) {
  IgnDebounce d = {};
  ignDebounce(d, raw, now, ON_MS, OFF_MS);
  return d;
}

// --- debounce -----------------------------------------------------------------

void test_first_sample_is_taken_as_is(void) {
  IgnDebounce on = booted(true, 100);
  TEST_ASSERT_TRUE(on.on);
  IgnDebounce off = booted(false, 100);
  TEST_ASSERT_FALSE(off.on);
  // Off from boot counts from boot: the boot-time check can power it down.
  TEST_ASSERT_EQUAL_UINT32(500, ignOffForMs(off, 600));
}

void test_never_sampled_is_not_off(void) {
  IgnDebounce d = {};
  TEST_ASSERT_EQUAL_UINT32(0, ignOffForMs(d, 123456));
}

void test_off_glitch_shorter_than_window_is_ignored(void) {
  IgnDebounce d = booted(true, 0);
  ignDebounce(d, false, 1000, ON_MS, OFF_MS);   // dip, e.g. cranking
  ignDebounce(d, false, 2500, ON_MS, OFF_MS);   // 1.5 s < 2 s
  ignDebounce(d, true, 2600, ON_MS, OFF_MS);    // recovers
  TEST_ASSERT_TRUE(d.on);
  // A new dip restarts the window from its own edge, not the old one.
  ignDebounce(d, false, 3000, ON_MS, OFF_MS);
  ignDebounce(d, false, 4900, ON_MS, OFF_MS);
  TEST_ASSERT_TRUE(d.on);
  TEST_ASSERT_EQUAL_UINT32(0, ignOffForMs(d, 4900));
}

void test_off_confirmed_after_window_counts_from_real_edge(void) {
  IgnDebounce d = booted(true, 0);
  ignDebounce(d, false, 1000, ON_MS, OFF_MS);
  ignDebounce(d, false, 3000, ON_MS, OFF_MS);   // exactly 2 s
  TEST_ASSERT_FALSE(d.on);
  TEST_ASSERT_EQUAL_UINT32(2000, ignOffForMs(d, 3000));
  TEST_ASSERT_EQUAL_UINT32(5000, ignOffForMs(d, 6000));
}

void test_on_is_quick(void) {
  IgnDebounce d = booted(false, 0);
  ignDebounce(d, true, 1000, ON_MS, OFF_MS);
  ignDebounce(d, true, 1150, ON_MS, OFF_MS);
  TEST_ASSERT_FALSE(d.on);
  ignDebounce(d, true, 1200, ON_MS, OFF_MS);
  TEST_ASSERT_TRUE(d.on);
  TEST_ASSERT_EQUAL_UINT32(0, ignOffForMs(d, 99999));
}

void test_debounce_across_millis_wrap(void) {
  IgnDebounce d = booted(true, 0xFFFFFF00u);
  ignDebounce(d, false, 0xFFFFFF80u, ON_MS, OFF_MS);
  ignDebounce(d, false, 0x00000800u, ON_MS, OFF_MS);   // ~2.2 s later
  TEST_ASSERT_FALSE(d.on);
  TEST_ASSERT_EQUAL_UINT32(0x880u, ignOffForMs(d, 0x00000800u));
}

// --- policy selection ---------------------------------------------------------

void test_names(void) {
  TEST_ASSERT_EQUAL_STRING("bus-quiet", powerPolicyName(POWER_BUS_QUIET));
  TEST_ASSERT_EQUAL_STRING("ignition", powerPolicyName(POWER_IGNITION));
}

void test_bus_quiet_policy_uses_bus_and_ignores_ignition(void) {
  IgnDebounce on = booted(true, 0), off = booted(false, 0);
  TEST_ASSERT_EQUAL_UINT32(4000, powerQuietMs(POWER_BUS_QUIET, 4000, on, 9000));
  TEST_ASSERT_EQUAL_UINT32(0, powerQuietMs(POWER_BUS_QUIET, 0, off, 9000));
  const PowerThresholds t =
      powerThresholds(POWER_BUS_QUIET, BUS_CLOSE, BUS_SLEEP, OFF_MS, IGN_SLEEP);
  TEST_ASSERT_EQUAL_UINT32(BUS_CLOSE, t.closeMs);
  TEST_ASSERT_EQUAL_UINT32(BUS_SLEEP, t.sleepMs);
  TEST_ASSERT_TRUE(powerMayRecord(POWER_BUS_QUIET, 999999, t));
}

// ⭐ The reason the policy exists: a silent bus with the ignition on must
// never power the board down, however long the silence.
void test_ignition_on_never_sleeps_on_bus_silence_alone(void) {
  IgnDebounce on = booted(true, 0);
  const PowerThresholds t =
      powerThresholds(POWER_IGNITION, BUS_CLOSE, BUS_SLEEP, OFF_MS, IGN_SLEEP);
  const uint32_t q = powerQuietMs(POWER_IGNITION, 0xFFFFFFF0u, on, 10000000);
  TEST_ASSERT_EQUAL_UINT32(0, q);
  TEST_ASSERT_TRUE(powerMayRecord(POWER_IGNITION, q, t));
  const SleepVerdict v = sleepVerdict(0, true, q, t.closeMs, t.sleepMs);
  TEST_ASSERT_FALSE(sleepShouldSleep(v));
}

void test_ignition_off_closes_then_sleeps(void) {
  IgnDebounce d = booted(true, 0);
  const PowerThresholds t =
      powerThresholds(POWER_IGNITION, BUS_CLOSE, BUS_SLEEP, OFF_MS, IGN_SLEEP);
  ignDebounce(d, false, 1000, ON_MS, OFF_MS);
  ignDebounce(d, false, 3000, ON_MS, OFF_MS);
  // Busy bus is irrelevant: the bus clock is not consulted.
  uint32_t q = powerQuietMs(POWER_IGNITION, 0, d, 3000);
  TEST_ASSERT_FALSE(powerMayRecord(POWER_IGNITION, q, t));     // close now
  TEST_ASSERT_EQUAL(SLEEP_REFUSED_FILES_OPEN,
                    sleepVerdict(2, true, q, t.closeMs, t.sleepMs));
  TEST_ASSERT_EQUAL(SLEEP_OK, sleepVerdict(0, true, q, t.closeMs, t.sleepMs));
  // At the power-down time a file still open is closed first.
  q = powerQuietMs(POWER_IGNITION, 0, d, 1000 + IGN_SLEEP);
  TEST_ASSERT_EQUAL(SLEEP_BACKSTOP_CLOSE_THEN_SLEEP,
                    sleepVerdict(1, true, q, t.closeMs, t.sleepMs));
}

void test_ignition_back_on_resumes_recording(void) {
  IgnDebounce d = booted(false, 0);
  const PowerThresholds t =
      powerThresholds(POWER_IGNITION, BUS_CLOSE, BUS_SLEEP, OFF_MS, IGN_SLEEP);
  TEST_ASSERT_FALSE(powerMayRecord(POWER_IGNITION,
                                   powerQuietMs(POWER_IGNITION, 0, d, 5000), t));
  ignDebounce(d, true, 5000, ON_MS, OFF_MS);
  ignDebounce(d, true, 5200, ON_MS, OFF_MS);
  TEST_ASSERT_TRUE(powerMayRecord(POWER_IGNITION,
                                  powerQuietMs(POWER_IGNITION, 0, d, 5200), t));
}

void test_zero_off_debounce_does_not_make_on_look_off(void) {
  const PowerThresholds t =
      powerThresholds(POWER_IGNITION, BUS_CLOSE, BUS_SLEEP, 0, IGN_SLEEP);
  TEST_ASSERT_EQUAL_UINT32(1, t.closeMs);
  TEST_ASSERT_TRUE(powerMayRecord(POWER_IGNITION, 0, t));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_first_sample_is_taken_as_is);
  RUN_TEST(test_never_sampled_is_not_off);
  RUN_TEST(test_off_glitch_shorter_than_window_is_ignored);
  RUN_TEST(test_off_confirmed_after_window_counts_from_real_edge);
  RUN_TEST(test_on_is_quick);
  RUN_TEST(test_debounce_across_millis_wrap);
  RUN_TEST(test_names);
  RUN_TEST(test_bus_quiet_policy_uses_bus_and_ignores_ignition);
  RUN_TEST(test_ignition_on_never_sleeps_on_bus_silence_alone);
  RUN_TEST(test_ignition_off_closes_then_sleeps);
  RUN_TEST(test_ignition_back_on_resumes_recording);
  RUN_TEST(test_zero_off_debounce_does_not_make_on_look_off);
  return UNITY_END();
}

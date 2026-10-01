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

// --- the TX gate ------------------------------------------------------------

void test_tx_gate(void) {
  TEST_ASSERT_TRUE(canTxGate(POWER_BUS_QUIET, false));     // unchanged: open
  TEST_ASSERT_TRUE(canTxGate(POWER_BUS_QUIET, true));
  TEST_ASSERT_TRUE(canTxGate(POWER_IGNITION, true));
  TEST_ASSERT_FALSE(canTxGate(POWER_IGNITION, false));     // never TX while off
}

void test_tx_gate_follows_debounced_ignition_through_a_crank_dip(void) {
  IgnDebounce d = booted(true, 0);
  // A 1.5 s dip (shorter than the OFF debounce) keeps the gate open.
  ignDebounce(d, false, 1000, ON_MS, OFF_MS);
  TEST_ASSERT_TRUE(canTxGate(POWER_IGNITION,
                             ignDebounce(d, false, 2400, ON_MS, OFF_MS)));
  ignDebounce(d, true, 2500, ON_MS, OFF_MS);
  // A sustained OFF shuts it.
  ignDebounce(d, false, 10000, ON_MS, OFF_MS);
  TEST_ASSERT_FALSE(canTxGate(POWER_IGNITION,
                              ignDebounce(d, false, 12000, ON_MS, OFF_MS)));
}

// --- wake source / sleep window ---------------------------------------------

void test_wake_source(void) {
  TEST_ASSERT_EQUAL(WAKE_NA, wakeSourceAtBoot(POWER_BUS_QUIET, true, true));
  TEST_ASSERT_EQUAL(WAKE_IGNITION, wakeSourceAtBoot(POWER_IGNITION, true, true));
  TEST_ASSERT_EQUAL(WAKE_IGNITION, wakeSourceAtBoot(POWER_IGNITION, true, false));
  TEST_ASSERT_EQUAL(WAKE_USB_BENCH, wakeSourceAtBoot(POWER_IGNITION, false, true));
  TEST_ASSERT_EQUAL(WAKE_BUS, wakeSourceAtBoot(POWER_IGNITION, false, false));
  TEST_ASSERT_EQUAL_STRING("bus-wake", wakeSourceName(WAKE_BUS));
  TEST_ASSERT_EQUAL_STRING("usb-bench", wakeSourceName(WAKE_USB_BENCH));
  TEST_ASSERT_EQUAL_STRING("ignition", wakeSourceName(WAKE_IGNITION));
}

void test_sleep_window(void) {
  TEST_ASSERT_EQUAL_UINT32(0, ignSleepAfterMs(true, false, 0, 10000));
  TEST_ASSERT_EQUAL_UINT32(5000, ignSleepAfterMs(true, true, 5000, 10000));
  TEST_ASSERT_EQUAL_UINT32(10000, ignSleepAfterMs(false, false, 0, 10000));
  // A watchdog reset with ignition off goes straight back to sleep.
  TEST_ASSERT_EQUAL_UINT32(0, ignSleepAfterMs(false, true, 0, 10000));
}

// --- TCAN1043 mode table ----------------------------------------------------

void test_xcvr_modes_never_normal(void) {
  const XcvrMode all[] = {XCVR_STANDBY, XCVR_SILENT, XCVR_GO_TO_SLEEP};
  for (XcvrMode m : all) {
    const XcvrPins p = xcvrPinsFor(m);
    TEST_ASSERT_FALSE(p.en && p.nstb);                     // normal = TX mode
  }
  TEST_ASSERT_FALSE(xcvrPinsFor(XCVR_SILENT).en);
  TEST_ASSERT_TRUE(xcvrPinsFor(XCVR_SILENT).nstb);
  TEST_ASSERT_TRUE(xcvrPinsFor(XCVR_GO_TO_SLEEP).en);
  TEST_ASSERT_FALSE(xcvrPinsFor(XCVR_GO_TO_SLEEP).nstb);
  TEST_ASSERT_FALSE(xcvrPinsFor(XCVR_STANDBY).en);
  TEST_ASSERT_FALSE(xcvrPinsFor(XCVR_STANDBY).nstb);
}

// --- close -> SAFE -> sleep ordering (sleepExecute) -------------------------

struct Fake {
  uint16_t open;
  bool closeWorks;
  char log[8];
  int n;
};
static void rec(Fake *f, char c) { if (f->n < 7) f->log[f->n++] = c; f->log[f->n] = 0; }
static uint16_t fOpen(void *c) { return ((Fake *)c)->open; }
static void fClose(void *c) {
  Fake *f = (Fake *)c;
  rec(f, 'C');
  if (f->closeWorks) f->open = 0;
}
static void fSafe(void *c) { rec((Fake *)c, 'S'); }
static void fSleep(void *c) { rec((Fake *)c, 'Z'); }
static SleepVerdict run(Fake &f, SleepVerdict v) {
  const SleepIo io = {&f, fOpen, fClose, fSafe, fSleep};
  return sleepExecute(v, io);
}

void test_ignition_off_closes_then_safe_then_sleep(void) {
  // Ignition OFF confirmed with two files open: the verdict is close-then-sleep.
  IgnDebounce d = booted(true, 0);
  ignDebounce(d, false, 1000, ON_MS, OFF_MS);
  ignDebounce(d, false, 3000, ON_MS, OFF_MS);
  const uint32_t q = powerQuietMs(POWER_IGNITION, 0, d, 3000);
  const SleepVerdict v = sleepVerdict(2, true, q, OFF_MS, OFF_MS);
  TEST_ASSERT_EQUAL(SLEEP_BACKSTOP_CLOSE_THEN_SLEEP, v);
  Fake f = {2, true, {0}, 0};
  run(f, v);
  TEST_ASSERT_EQUAL_STRING("CSZ", f.log);
}

void test_never_sleep_when_close_fails(void) {
  Fake f = {1, false, {0}, 0};
  TEST_ASSERT_EQUAL(SLEEP_REFUSED_FILES_OPEN,
                    run(f, SLEEP_BACKSTOP_CLOSE_THEN_SLEEP));
  TEST_ASSERT_EQUAL_STRING("C", f.log);                    // no S, no Z
}

void test_refusal_commands_nothing(void) {
  Fake f = {1, true, {0}, 0};
  run(f, SLEEP_REFUSED_FILES_OPEN);
  TEST_ASSERT_EQUAL_STRING("", f.log);
}

void test_nothing_open_safe_then_sleep(void) {
  Fake f = {0, true, {0}, 0};
  TEST_ASSERT_EQUAL(SLEEP_OK, run(f, SLEEP_OK));
  TEST_ASSERT_EQUAL_STRING("SZ", f.log);
}

// --- mode restore across OFF -> ON ------------------------------------------

static const uint8_t M_LISTEN = 0, M_POLL = 3;   // modes are opaque here

void test_off_then_on_within_sleep_window_restores_mode(void) {
  const uint32_t SLEEP_AFTER = 5000;               // extra wait after OFF
  ModeRestore r = {MODE_RESTORE_NONE};
  IgnDebounce d = booted(true, 0);
  uint8_t mode = M_POLL;
  // Ignition drops at 1000; OFF believed at 3000 -> POLL dropped to LISTEN.
  ignDebounce(d, false, 1000, ON_MS, OFF_MS);
  TEST_ASSERT_FALSE(ignDebounce(d, false, 3000, ON_MS, OFF_MS));
  modeRestoreOnOff(r, mode, true);
  mode = M_LISTEN;
  TEST_ASSERT_FALSE(canTxGate(POWER_IGNITION, d.on));
  // Back ON at 4000, believed at 4200: still inside OFF_MS + SLEEP_AFTER.
  ignDebounce(d, true, 4000, ON_MS, OFF_MS);
  TEST_ASSERT_TRUE(ignDebounce(d, true, 4200, ON_MS, OFF_MS));
  TEST_ASSERT_TRUE(4200 - 1000 < OFF_MS + SLEEP_AFTER);
  const uint8_t m = modeRestoreOnOn(r, canTxGate(POWER_IGNITION, d.on));
  TEST_ASSERT_EQUAL_UINT8(M_POLL, m);
  // One-shot: the next ON edge restores nothing.
  TEST_ASSERT_EQUAL_UINT8(MODE_RESTORE_NONE, modeRestoreOnOn(r, true));
}

void test_restore_needs_the_gate_open(void) {
  ModeRestore r = {MODE_RESTORE_NONE};
  modeRestoreOnOff(r, M_POLL, true);
  TEST_ASSERT_EQUAL_UINT8(MODE_RESTORE_NONE, modeRestoreOnOn(r, false));
  TEST_ASSERT_EQUAL_UINT8(M_POLL, modeRestoreOnOn(r, true));   // kept until allowed
}

void test_restore_only_remembers_transmitting_modes(void) {
  ModeRestore r = {MODE_RESTORE_NONE};
  modeRestoreOnOff(r, M_LISTEN, false);
  TEST_ASSERT_EQUAL_UINT8(MODE_RESTORE_NONE, modeRestoreOnOn(r, true));
}

void test_second_off_keeps_the_configured_mode(void) {
  ModeRestore r = {MODE_RESTORE_NONE};
  modeRestoreOnOff(r, M_POLL, true);
  modeRestoreOnOff(r, 2, true);
  TEST_ASSERT_EQUAL_UINT8(M_POLL, modeRestoreOnOn(r, true));
}

void test_deliberate_change_cancels_restore(void) {
  ModeRestore r = {MODE_RESTORE_NONE};
  modeRestoreOnOff(r, M_POLL, true);
  modeRestoreCancel(r);
  TEST_ASSERT_EQUAL_UINT8(MODE_RESTORE_NONE, modeRestoreOnOn(r, true));
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
  RUN_TEST(test_tx_gate);
  RUN_TEST(test_tx_gate_follows_debounced_ignition_through_a_crank_dip);
  RUN_TEST(test_wake_source);
  RUN_TEST(test_sleep_window);
  RUN_TEST(test_xcvr_modes_never_normal);
  RUN_TEST(test_ignition_off_closes_then_safe_then_sleep);
  RUN_TEST(test_never_sleep_when_close_fails);
  RUN_TEST(test_refusal_commands_nothing);
  RUN_TEST(test_nothing_open_safe_then_sleep);
  RUN_TEST(test_off_then_on_within_sleep_window_restores_mode);
  RUN_TEST(test_restore_needs_the_gate_open);
  RUN_TEST(test_restore_only_remembers_transmitting_modes);
  RUN_TEST(test_second_off_keeps_the_configured_mode);
  RUN_TEST(test_deliberate_change_cancels_restore);
  return UNITY_END();
}

// scansched.h: scan for the hub every 4 s, every 30 s after 2 min unseen.

#include <unity.h>

#include "scansched.h"

static ScanSched s;
static void fresh(void) { scanSchedInit(&s, 4000, 30000, 120000, 4); }

void setUp(void) {}
void tearDown(void) {}

// Run the schedule for `ms`, scanning whenever due; returns scans started.
static uint32_t run(uint32_t from, uint32_t ms, uint32_t *times, uint32_t cap) {
  uint32_t n = 0;
  for (uint32_t t = from; t <= from + ms; t += 100) {
    if (scanSchedDue(&s, t)) {
      scanSchedStart(&s, t, 6);
      if (n < cap) times[n] = t;
      n++;
    }
  }
  return n;
}

void test_first_scan_is_immediate_after_a_disconnect(void) {
  fresh();
  scanSchedReset(&s, 50000);
  TEST_ASSERT_TRUE(scanSchedDue(&s, 50000));
}

void test_fast_every_4s_for_two_minutes_then_every_30s(void) {
  fresh();
  scanSchedReset(&s, 0);
  uint32_t t[200];
  const uint32_t n = run(0, 120000 - 100, t, 200);
  TEST_ASSERT_EQUAL_UINT32(30, n);               // 0, 4, 8 ... 116 s
  TEST_ASSERT_EQUAL_UINT32(4000, t[1] - t[0]);
  // After 2 min unseen: 30 s apart.
  const uint32_t m = run(120000, 120000, t, 200);
  TEST_ASSERT_TRUE(m >= 4 && m <= 5);
  TEST_ASSERT_EQUAL_UINT32(30000, t[2] - t[1]);
}

void test_a_sighting_restarts_the_fast_window(void) {
  fresh();
  scanSchedReset(&s, 0);
  uint32_t t[64];
  run(0, 200000, t, 64);                         // now slow
  TEST_ASSERT_EQUAL_UINT32(30000, scanSchedInterval(&s, 200000));
  scanSchedSighted(&s, 200000);
  TEST_ASSERT_EQUAL_UINT32(4000, scanSchedInterval(&s, 200000));
}

void test_a_disconnect_resets_to_fast_and_immediate(void) {
  fresh();
  scanSchedReset(&s, 0);
  uint32_t t[64];
  run(0, 300000, t, 64);
  scanSchedReset(&s, 300050);
  TEST_ASSERT_TRUE(scanSchedDue(&s, 300050));
  TEST_ASSERT_EQUAL_UINT32(4000, scanSchedInterval(&s, 300050));
}

void test_known_channel_scanned_with_a_full_sweep_every_4th(void) {
  fresh();
  scanSchedReset(&s, 0);
  uint8_t ch[8];
  for (int i = 0; i < 8; i++) ch[i] = scanSchedStart(&s, i * 4000, 6);
  const uint8_t want[8] = {0, 6, 6, 6, 0, 6, 6, 6};
  for (int i = 0; i < 8; i++) TEST_ASSERT_EQUAL_UINT8(want[i], ch[i]);
  fresh();
  scanSchedReset(&s, 0);
  TEST_ASSERT_EQUAL_UINT8(0, scanSchedStart(&s, 0, 0));   // unknown: all
  TEST_ASSERT_EQUAL_UINT8(0, scanSchedStart(&s, 4000, 0));
}

void test_millis_wrap(void) {
  fresh();
  scanSchedReset(&s, 0xFFFFF000u);
  scanSchedStart(&s, 0xFFFFF000u, 6);
  TEST_ASSERT_FALSE(scanSchedDue(&s, 0xFFFFF000u + 3000));
  TEST_ASSERT_TRUE(scanSchedDue(&s, 0xFFFFF000u + 4000));  // wraps past 0
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_first_scan_is_immediate_after_a_disconnect);
  RUN_TEST(test_fast_every_4s_for_two_minutes_then_every_30s);
  RUN_TEST(test_a_sighting_restarts_the_fast_window);
  RUN_TEST(test_a_disconnect_resets_to_fast_and_immediate);
  RUN_TEST(test_known_channel_scanned_with_a_full_sweep_every_4th);
  RUN_TEST(test_millis_wrap);
  return UNITY_END();
}

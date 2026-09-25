// fsprof.h: exclusive time per filestore sub-stage, host-tested.

#include <unity.h>

#include "fsprof.h"

static FsProf p;

void setUp(void) {}
void tearDown(void) {}

void test_nested_time_is_charged_to_the_inner_stage(void) {
  // snapshot 0..1000 us, inside it rotate 100..900, inside that fs_size
  // 200..800. Exclusive: snapshot 200, rotate 200, fs_size 600.
  fsProfBeginPass(&p);
  fsProfEnter(&p, FS_SUB_SNAPSHOT, 0);
  fsProfEnter(&p, FS_SUB_ROTATE, 100);
  fsProfEnter(&p, FS_SUB_FSSIZE, 200);
  fsProfExit(&p, 800);
  fsProfExit(&p, 900);
  fsProfExit(&p, 1000);
  TEST_ASSERT_EQUAL_UINT32(200, p.acc[FS_SUB_SNAPSHOT]);
  TEST_ASSERT_EQUAL_UINT32(200, p.acc[FS_SUB_ROTATE]);
  TEST_ASSERT_EQUAL_UINT32(600, p.acc[FS_SUB_FSSIZE]);
  uint32_t us = 0;
  TEST_ASSERT_EQUAL_UINT8(FS_SUB_FSSIZE, fsProfEndPass(&p, &us));
  TEST_ASSERT_EQUAL_UINT32(600, us);
  TEST_ASSERT_EQUAL_STRING("fs_size", fsSubName(FS_SUB_FSSIZE));
}

void test_repeated_stages_accumulate_within_a_pass(void) {
  fsProfBeginPass(&p);
  for (uint32_t i = 0; i < 4; i++) {        // four fs_size walks, 250 us each
    fsProfEnter(&p, FS_SUB_FSSIZE, i * 1000);
    fsProfExit(&p, i * 1000 + 250);
  }
  fsProfEnter(&p, FS_SUB_FLUSH, 5000);
  fsProfExit(&p, 5600);
  uint32_t us = 0;
  TEST_ASSERT_EQUAL_UINT8(FS_SUB_FSSIZE, fsProfEndPass(&p, &us));
  TEST_ASSERT_EQUAL_UINT32(1000, us);
}

void test_outside_a_pass_nothing_is_recorded(void) {
  fsProfBeginPass(&p);
  uint32_t us;
  fsProfEndPass(&p, &us);
  // e.g. a web handler hydrating on demand: not part of the tick.
  fsProfEnter(&p, FS_SUB_HYDRATE, 0);
  fsProfExit(&p, 5000);
  TEST_ASSERT_EQUAL_UINT32(0, p.acc[FS_SUB_HYDRATE]);
}

void test_micros_wrap_inside_a_stage(void) {
  fsProfBeginPass(&p);
  fsProfEnter(&p, FS_SUB_RETENTION, 0xFFFFFF00u);
  fsProfExit(&p, 0x00000100u);
  TEST_ASSERT_EQUAL_UINT32(0x200, p.acc[FS_SUB_RETENTION]);
}

void test_too_deep_stays_balanced(void) {
  fsProfBeginPass(&p);
  for (uint32_t i = 0; i < FS_PROF_DEPTH + 3; i++)
    fsProfEnter(&p, FS_SUB_ROTATE, i);
  for (uint32_t i = 0; i < FS_PROF_DEPTH + 3; i++)
    fsProfExit(&p, 100 + i);
  TEST_ASSERT_EQUAL_UINT8(0, p.depth);
  // A fresh stage afterwards is still timed correctly.
  fsProfEnter(&p, FS_SUB_FLUSH, 1000);
  fsProfExit(&p, 1300);
  TEST_ASSERT_EQUAL_UINT32(300, p.acc[FS_SUB_FLUSH]);
}

void test_window_keeps_the_worst_pass(void) {
  FsSubWindow w = {0, 0, 0};
  fsSubWindowNote(&w, FS_SUB_FLUSH, 30000, 40000);
  fsSubWindowNote(&w, FS_SUB_FSSIZE, 900000, 1400000);
  fsSubWindowNote(&w, FS_SUB_SNAPSHOT, 50000, 60000);
  TEST_ASSERT_EQUAL_UINT8(FS_SUB_FSSIZE, w.worstSub);
  TEST_ASSERT_EQUAL_UINT32(900000, w.worstUs);
  TEST_ASSERT_EQUAL_UINT32(1400000, w.passUs);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_nested_time_is_charged_to_the_inner_stage);
  RUN_TEST(test_repeated_stages_accumulate_within_a_pass);
  RUN_TEST(test_outside_a_pass_nothing_is_recorded);
  RUN_TEST(test_micros_wrap_inside_a_stage);
  RUN_TEST(test_too_deep_stays_balanced);
  RUN_TEST(test_window_keeps_the_worst_pass);
  return UNITY_END();
}

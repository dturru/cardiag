// stackguard.h: keep the loop stack's lowest high-water mark and warn once.

#include <unity.h>

#include "stackguard.h"

static StackGuard g;

void setUp(void) { stackGuardInit(&g); }
void tearDown(void) {}

void test_tracks_the_minimum(void) {
  stackGuardSample(&g, 3000, STACKGUARD_MARGIN_BYTES);
  stackGuardSample(&g, 2500, STACKGUARD_MARGIN_BYTES);
  stackGuardSample(&g, 2800, STACKGUARD_MARGIN_BYTES);
  TEST_ASSERT_EQUAL_UINT32(2500, g.minFree);
}

void test_warns_once_below_margin(void) {
  TEST_ASSERT_FALSE(stackGuardSample(&g, 2000, STACKGUARD_MARGIN_BYTES));
  TEST_ASSERT_TRUE(stackGuardSample(&g, 900, STACKGUARD_MARGIN_BYTES));
  TEST_ASSERT_FALSE(stackGuardSample(&g, 800, STACKGUARD_MARGIN_BYTES));
  TEST_ASSERT_FALSE(stackGuardSample(&g, 2000, STACKGUARD_MARGIN_BYTES));
  TEST_ASSERT_EQUAL_UINT32(800, g.minFree);
}

void test_exactly_at_margin_is_not_a_warning(void) {
  TEST_ASSERT_FALSE(stackGuardSample(&g, STACKGUARD_MARGIN_BYTES, STACKGUARD_MARGIN_BYTES));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_tracks_the_minimum);
  RUN_TEST(test_warns_once_below_margin);
  RUN_TEST(test_exactly_at_margin_is_not_a_warning);
  return UNITY_END();
}

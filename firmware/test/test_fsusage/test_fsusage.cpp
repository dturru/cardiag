// fsusage.h: LittleFS usage kept in RAM from one boot measurement.

#include <unity.h>

#include "fsusage.h"

static FsUsage u;

static void fresh(void) {
  u = FsUsage{4096, 512, 4063232, 0, false};
  fsUsageMeasured(&u, 4063232, 409600);          // 100 blocks at boot
}

void setUp(void) {}
void tearDown(void) {}

void test_small_files_are_inline_and_cost_nothing(void) {
  fresh();
  TEST_ASSERT_EQUAL_UINT32(0, fsDataBlocks(&u, 0));
  TEST_ASSERT_EQUAL_UINT32(0, fsDataBlocks(&u, 120));     // a .meta sidecar
  TEST_ASSERT_EQUAL_UINT32(0, fsDataBlocks(&u, 512));
  TEST_ASSERT_EQUAL_UINT32(1, fsDataBlocks(&u, 513));
  TEST_ASSERT_EQUAL_UINT32(1, fsDataBlocks(&u, 4096));
  TEST_ASSERT_EQUAL_UINT32(2, fsDataBlocks(&u, 4097));
}

void test_growth_charges_only_block_crossings(void) {
  fresh();
  uint32_t bytes = 0;
  // A Tier A file written 1 KB at a time up to 64 KB: 16 blocks, whatever
  // the write size.
  for (int i = 0; i < 64; i++) {
    fsUsageResize(&u, bytes, bytes + 1024);
    bytes += 1024;
  }
  TEST_ASSERT_EQUAL_UINT32(409600 + 16 * 4096, u.usedBytes);
}

void test_remove_gives_back_exactly_what_growth_took(void) {
  fresh();
  const uint32_t before = u.usedBytes;
  uint32_t bytes = 0;
  const uint32_t steps[] = {100, 700, 3000, 5000, 20000, 20001};
  for (uint32_t s : steps) {
    fsUsageResize(&u, bytes, s);
    bytes = s;
  }
  fsUsageResize(&u, bytes, 0);                     // the file is deleted
  TEST_ASSERT_EQUAL_UINT32(before, u.usedBytes);
}

void test_nothing_is_counted_before_the_boot_measurement(void) {
  u = FsUsage{4096, 512, 0, 0, false};
  fsUsageResize(&u, 0, 100000);                     // a scan-time eviction...
  fsUsageResize(&u, 100000, 0);
  TEST_ASSERT_EQUAL_UINT32(0, u.usedBytes);         // ...changed nothing
  fsUsageMeasured(&u, 4063232, 3600384);
  TEST_ASSERT_EQUAL_UINT8(88, fsUsagePct(&u));
}

void test_never_underflows_or_exceeds_total(void) {
  fresh();
  fsUsageResize(&u, 4063232, 0);                     // remove more than used
  TEST_ASSERT_EQUAL_UINT32(0, u.usedBytes);
  fresh();
  fsUsageResize(&u, 0, 0xFFFFFFF0u);                 // absurd growth
  TEST_ASSERT_EQUAL_UINT32(u.totalBytes, u.usedBytes);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_small_files_are_inline_and_cost_nothing);
  RUN_TEST(test_growth_charges_only_block_crossings);
  RUN_TEST(test_remove_gives_back_exactly_what_growth_took);
  RUN_TEST(test_nothing_is_counted_before_the_boot_measurement);
  RUN_TEST(test_never_underflows_or_exceeds_total);
  return UNITY_END();
}

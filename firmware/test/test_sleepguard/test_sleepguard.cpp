// The go-to-sleep invariant, tested on the host.
//
// This is the rule that replaces a timing margin that was never real: INH does
// not drop on its own in normal mode, so the only way an open file loses its
// bytes at key-off is if this firmware commands sleep while holding one.

#include <unity.h>

#include "sleepguard.h"

static const uint32_t QUIET = 3000;   // CAN_BUS_IDLE_CLOSE_MS

// --- the invariant ---------------------------------------------------------

void test_sleep_is_refused_while_a_file_is_open(void) {
  // Bus long quiet, storage fine -- and it STILL refuses, because a file is
  // open. This is the whole point.
  TEST_ASSERT_EQUAL(SLEEP_REFUSED_FILES_OPEN,
                    sleepVerdict(1, true, 60000, QUIET));
}

void test_open_files_outrank_a_quiet_bus_however_long(void) {
  TEST_ASSERT_EQUAL(SLEEP_REFUSED_FILES_OPEN,
                    sleepVerdict(1, true, 0xFFFFFFFFu, QUIET));
}

void test_many_open_files_still_refuse(void) {
  TEST_ASSERT_EQUAL(SLEEP_REFUSED_FILES_OPEN,
                    sleepVerdict(2, true, 60000, QUIET));
}

void test_sleep_allowed_once_everything_is_closed(void) {
  TEST_ASSERT_EQUAL(SLEEP_OK, sleepVerdict(0, true, QUIET, QUIET));
}

// --- the debounce ----------------------------------------------------------

void test_a_busy_bus_refuses_even_with_no_files_open(void) {
  TEST_ASSERT_EQUAL(SLEEP_REFUSED_BUS_ACTIVE,
                    sleepVerdict(0, true, 0, QUIET));
}

void test_the_quiet_threshold_is_inclusive(void) {
  TEST_ASSERT_EQUAL(SLEEP_REFUSED_BUS_ACTIVE,
                    sleepVerdict(0, true, QUIET - 1, QUIET));
  TEST_ASSERT_EQUAL(SLEEP_OK, sleepVerdict(0, true, QUIET, QUIET));
}

// --- unknown is not "fine" -------------------------------------------------

void test_an_unmounted_filestore_refuses(void) {
  // Zero open files on a filestore that never mounted is not evidence that
  // nothing is at risk -- it is an absence of evidence, and it must not read
  // as permission.
  TEST_ASSERT_EQUAL(SLEEP_REFUSED_FS_UNKNOWN,
                    sleepVerdict(0, false, 60000, QUIET));
}

void test_unmounted_outranks_everything(void) {
  TEST_ASSERT_EQUAL(SLEEP_REFUSED_FS_UNKNOWN,
                    sleepVerdict(5, false, 0, QUIET));
}

// --- the reasons are usable in a log --------------------------------------

void test_every_verdict_has_a_distinct_readable_reason(void) {
  TEST_ASSERT_EQUAL_STRING("ok", sleepVerdictName(SLEEP_OK));
  TEST_ASSERT_EQUAL_STRING("refused: files still open",
                           sleepVerdictName(SLEEP_REFUSED_FILES_OPEN));
  TEST_ASSERT_EQUAL_STRING("refused: bus still active",
                           sleepVerdictName(SLEEP_REFUSED_BUS_ACTIVE));
  TEST_ASSERT_EQUAL_STRING("refused: filestore not mounted",
                           sleepVerdictName(SLEEP_REFUSED_FS_UNKNOWN));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_sleep_is_refused_while_a_file_is_open);
  RUN_TEST(test_open_files_outrank_a_quiet_bus_however_long);
  RUN_TEST(test_many_open_files_still_refuse);
  RUN_TEST(test_sleep_allowed_once_everything_is_closed);
  RUN_TEST(test_a_busy_bus_refuses_even_with_no_files_open);
  RUN_TEST(test_the_quiet_threshold_is_inclusive);
  RUN_TEST(test_an_unmounted_filestore_refuses);
  RUN_TEST(test_unmounted_outranks_everything);
  RUN_TEST(test_every_verdict_has_a_distinct_readable_reason);
  return UNITY_END();
}

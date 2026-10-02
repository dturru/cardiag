// pollplan.h: the POST /api/v1/pollplan contract (carhub docs/protocol.md),
// the shared hash vector, the NVS encoding and the scheduler.

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "pollplan.h"

void setUp(void) {}
void tearDown(void) {}

static PollPlanResult parse(const char *s, PollPlan *p) {
  return pollPlanParse(s, strlen(s), p);
}

// ⭐ SHARED VECTOR -- the same bytes are in carhub's tests. Entries are given
// OUT of order; the canonical form sorts them by (mode, pid).
static const char kVectorBody[] =
    "{ \"entries\": [ {\"pid\": 13, \"mode\": 1, \"period_ms\": 500},"
    " {\"mode\":1,\"pid\":5,\"period_ms\":1000},"
    " {\"period_ms\":200,\"pid\":12,\"mode\":1} ], \"version\": 1 }";
static const char kVectorCanon[] =
    "{\"version\":1,\"entries\":[{\"mode\":1,\"pid\":5,\"period_ms\":1000},"
    "{\"mode\":1,\"pid\":12,\"period_ms\":200},"
    "{\"mode\":1,\"pid\":13,\"period_ms\":500}]}";
static const char kVectorHash[] = "3046906ac3000655";
static const char kEmptyHash[]  = "8bcae181d21321b7";   // {"version":1,"entries":[]}

void test_shared_vector_canonical_and_hash(void) {
  PollPlan p;
  TEST_ASSERT_EQUAL(POLLPLAN_OK, parse(kVectorBody, &p).status);
  TEST_ASSERT_EQUAL_UINT8(3, p.n);
  char canon[POLLPLAN_CANON_MAX];
  const size_t n = pollPlanCanonical(p, canon, sizeof(canon));
  TEST_ASSERT_EQUAL(strlen(kVectorCanon), n);
  TEST_ASSERT_EQUAL_STRING(kVectorCanon, canon);
  char h[POLL_HASH_HEX + 1];
  pollPlanHash(p, h);
  TEST_ASSERT_EQUAL_STRING(kVectorHash, h);
}

// ⭐ carhub's vector (carhub docs/protocol.md §2.6), fed in its canonical form.
void test_carhub_plan_vector(void) {
  static const char kCanon[] =
      "{\"version\":1,\"entries\":[{\"mode\":1,\"pid\":5,\"period_ms\":2000},"
      "{\"mode\":1,\"pid\":12,\"period_ms\":200},"
      "{\"mode\":1,\"pid\":13,\"period_ms\":500}]}";
  PollPlan p;
  TEST_ASSERT_EQUAL(POLLPLAN_OK, parse(kCanon, &p).status);
  char canon[POLLPLAN_CANON_MAX];
  pollPlanCanonical(p, canon, sizeof(canon));
  TEST_ASSERT_EQUAL_STRING(kCanon, canon);           // canonical form is a fixed point
  char h[POLL_HASH_HEX + 1];
  pollPlanHash(p, h);
  TEST_ASSERT_EQUAL_STRING("e52fa6872718765d", h);
}

void test_empty_entries_is_a_valid_clear(void) {
  PollPlan p;
  TEST_ASSERT_EQUAL(POLLPLAN_OK, parse("{\"version\":1,\"entries\":[]}", &p).status);
  TEST_ASSERT_EQUAL_UINT8(0, p.n);
  char h[POLL_HASH_HEX + 1];
  pollPlanHash(p, h);
  TEST_ASSERT_EQUAL_STRING(kEmptyHash, h);
}

void test_malformed_json_is_400(void) {
  const char *bad[] = {
    "", "{", "{\"version\":1,\"entries\":[}", "{\"version\":1,}", "[1,]",
    "{\"version\":01,\"entries\":[]}", "{\"version\":1 \"entries\":[]}",
    "{\"version\":1,\"entries\":[]} x", "{'version':1}", "{\"a\":tru}",
    "{\"a\":\"\\q\"}", "{\"a\":-}",
  };
  for (const char *s : bad) {
    PollPlan p;
    TEST_ASSERT_EQUAL_MESSAGE(POLLPLAN_MALFORMED, parse(s, &p).status, s);
  }
}

static void expect422(const char *body, const char *reason) {
  PollPlan p;
  const PollPlanResult r = parse(body, &p);
  TEST_ASSERT_EQUAL_MESSAGE(POLLPLAN_INVALID, r.status, body);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(reason, r.error, body);
}

void test_contract_violations_are_422(void) {
  expect422("{\"version\":1,\"entries\":[{\"mode\":34,\"pid\":5,\"period_ms\":1000}]}",
            "unsupported mode");
  expect422("{\"version\":1,\"entries\":[{\"mode\":4,\"pid\":0,\"period_ms\":1000}]}",
            "unsupported mode");
  expect422("{\"version\":1,\"entries\":[{\"mode\":1,\"pid\":256,\"period_ms\":1000}]}",
            "pid out of range");
  expect422("{\"version\":1,\"entries\":[{\"mode\":1,\"pid\":-1,\"period_ms\":1000}]}",
            "pid out of range");
  expect422("{\"version\":1,\"entries\":[{\"mode\":1,\"pid\":5,\"period_ms\":99}]}",
            "period_ms out of range");
  expect422("{\"version\":1,\"entries\":[{\"mode\":1,\"pid\":5,\"period_ms\":60001}]}",
            "period_ms out of range");
  expect422("{\"version\":1,\"entries\":[{\"mode\":1,\"pid\":5,\"period_ms\":100},"
            "{\"mode\":1,\"pid\":5,\"period_ms\":200}]}", "duplicate entry");
  expect422("{\"version\":2,\"entries\":[]}", "unsupported version");
  expect422("{\"entries\":[]}", "missing field");
  expect422("{\"version\":1}", "missing field");
  expect422("{\"version\":1,\"entries\":[{\"mode\":1,\"pid\":5}]}", "missing field");
  expect422("{\"version\":1,\"entries\":[{\"mode\":1,\"pid\":5,\"period_ms\":100,"
            "\"did\":61840}]}", "unknown field");
  expect422("{\"version\":1,\"entries\":[{\"mode\":\"1\",\"pid\":5,\"period_ms\":100}]}",
            "invalid type");
  expect422("{\"version\":1,\"entries\":[{\"mode\":1,\"pid\":5.0,\"period_ms\":100}]}",
            "invalid type");
  expect422("[]", "body must be an object");
}

void test_boundaries_accepted(void) {
  PollPlan p;
  TEST_ASSERT_EQUAL(POLLPLAN_OK,
      parse("{\"version\":1,\"entries\":[{\"mode\":1,\"pid\":0,\"period_ms\":100},"
            "{\"mode\":1,\"pid\":255,\"period_ms\":60000}]}", &p).status);
  TEST_ASSERT_EQUAL_UINT8(2, p.n);
}

void test_32_entries_ok_33_refused(void) {
  char body[POLL_JSON_MAX];
  size_t n = (size_t)snprintf(body, sizeof(body), "{\"version\":1,\"entries\":[");
  for (int i = 0; i < 33; i++)
    n += (size_t)snprintf(body + n, sizeof(body) - n,
                          "%s{\"mode\":1,\"pid\":%d,\"period_ms\":1000}", i ? "," : "", i);
  snprintf(body + n, sizeof(body) - n, "]}");
  expect422(body, "too many entries");
  // Drop the 33rd: 32 is the cap, inclusive.
  char *last = strrchr(body, '{');
  snprintf(last - 1, 4, "]}");
  PollPlan p;
  TEST_ASSERT_EQUAL(POLLPLAN_OK, parse(body, &p).status);
  TEST_ASSERT_EQUAL_UINT8(32, p.n);
}

void test_nvs_encoding_round_trip_and_rejects(void) {
  PollPlan p, q;
  TEST_ASSERT_EQUAL(POLLPLAN_OK, parse(kVectorBody, &p).status);
  uint8_t blob[POLLPLAN_BLOB_MAX];
  const size_t n = pollPlanEncode(p, blob, sizeof(blob));
  TEST_ASSERT_EQUAL(2 + 3 * 6, n);
  TEST_ASSERT_TRUE(pollPlanDecode(blob, n, &q));
  TEST_ASSERT_EQUAL_MEMORY(&p.e, &q.e, sizeof(PollEntry) * 3);
  TEST_ASSERT_FALSE(pollPlanDecode(blob, n - 1, &q));     // short
  blob[0] = 2;
  TEST_ASSERT_FALSE(pollPlanDecode(blob, n, &q));         // version
  blob[0] = 1;
  blob[2] = 0x22;                                         // mode tampered
  TEST_ASSERT_FALSE(pollPlanDecode(blob, n, &q));
}

// --- scheduler ----------------------------------------------------------------

static PollPlan twoEntries() {
  PollPlan p;
  parse("{\"version\":1,\"entries\":[{\"mode\":1,\"pid\":12,\"period_ms\":200},"
        "{\"mode\":1,\"pid\":13,\"period_ms\":1000}]}", &p);
  return p;
}

static void reply(PollSched &s, uint8_t pid, uint32_t now) {
  const uint8_t f[8] = {0x04, 0x41, pid, 0x1A, 0xF8, 0, 0, 0};
  uint8_t pl[8], len = 0;
  TEST_ASSERT_EQUAL(POLL_RX_OK, pollSchedOnFrame(s, f, 8, now, pl, &len));
  TEST_ASSERT_EQUAL_UINT8(2, len);
  TEST_ASSERT_EQUAL_HEX8(0x1A, pl[0]);
}

void test_one_request_in_flight(void) {
  PollSched s;
  pollSchedInit(s, twoEntries(), 0);
  const int i = pollSchedPick(s, 0, 10);
  TEST_ASSERT_EQUAL(0, i);
  pollSchedSent(s, i, 0);
  TEST_ASSERT_EQUAL(-1, pollSchedPick(s, 50, 10));       // still waiting
  reply(s, 12, 30);
  TEST_ASSERT_EQUAL(1, pollSchedPick(s, 40, 10));        // the other one, after the gap
  TEST_ASSERT_EQUAL(-1, pollSchedPick(s, 5, 10));        // gap not elapsed
  TEST_ASSERT_TRUE(s.anyOk);
}

void test_reply_for_another_pid_is_ignored(void) {
  PollSched s;
  pollSchedInit(s, twoEntries(), 0);
  pollSchedSent(s, 0, 0);                                // pid 12 in flight
  const uint8_t wrongPid[8] = {0x04, 0x41, 13, 0, 0, 0, 0, 0};
  const uint8_t wrongMode[8] = {0x04, 0x42, 12, 0, 0, 0, 0, 0};
  const uint8_t cf[8] = {0x21, 0x41, 12, 0, 0, 0, 0, 0};
  TEST_ASSERT_EQUAL(POLL_RX_IGNORED, pollSchedOnFrame(s, wrongPid, 8, 5, nullptr, nullptr));
  TEST_ASSERT_EQUAL(POLL_RX_IGNORED, pollSchedOnFrame(s, wrongMode, 8, 5, nullptr, nullptr));
  TEST_ASSERT_EQUAL(POLL_RX_IGNORED, pollSchedOnFrame(s, cf, 8, 5, nullptr, nullptr));
  TEST_ASSERT_EQUAL(0, s.inflight);
}

void test_cadence_from_send_time(void) {
  PollSched s;
  pollSchedInit(s, twoEntries(), 0);
  pollSchedSent(s, 0, 0);
  reply(s, 12, 80);                                      // slow reply
  TEST_ASSERT_EQUAL_UINT32(200, s.due[0]);               // not 280
  TEST_ASSERT_EQUAL_UINT32(80, s.st[0].lastLatencyMs);
}

void test_most_overdue_goes_first(void) {
  PollSched s;
  pollSchedInit(s, twoEntries(), 0);
  s.due[0] = 900;
  s.due[1] = 500;
  TEST_ASSERT_EQUAL(1, pollSchedPick(s, 1000, 10));
}

void test_timeout_backs_off_and_caps(void) {
  PollSched s;
  pollSchedInit(s, twoEntries(), 0);
  pollSchedSent(s, 0, 0);
  TEST_ASSERT_FALSE(pollSchedCheckTimeout(s, 99, 100, 5000));
  TEST_ASSERT_TRUE(pollSchedCheckTimeout(s, 100, 100, 5000));
  TEST_ASSERT_EQUAL(-1, s.inflight);
  TEST_ASSERT_EQUAL_UINT32(1, s.st[0].timeouts);
  TEST_ASSERT_EQUAL_UINT32(100 + 400, s.due[0]);         // 200 << 1
  TEST_ASSERT_EQUAL_UINT32(800, pollBackoffMs(200, 2, 5000));
  TEST_ASSERT_EQUAL_UINT32(5000, pollBackoffMs(200, 10, 5000));
  TEST_ASSERT_EQUAL_UINT32(60000, pollBackoffMs(60000, 3, 5000));  // never below period
  // A reply clears the miss count.
  pollSchedSent(s, 0, 500);
  reply(s, 12, 510);
  TEST_ASSERT_EQUAL_UINT8(0, s.st[0].misses);
}

void test_tx_refused_is_not_a_timeout(void) {
  PollSched s;
  pollSchedInit(s, twoEntries(), 0);
  pollSchedTxFailed(s, 0, 0);
  TEST_ASSERT_EQUAL_UINT32(1, s.st[0].txFailed);
  TEST_ASSERT_EQUAL_UINT32(0, s.st[0].timeouts);
  TEST_ASSERT_EQUAL_UINT8(0, s.st[0].misses);
  TEST_ASSERT_EQUAL_UINT32(200, s.due[0]);
  TEST_ASSERT_EQUAL(-1, s.inflight);
}

void test_first_frame_counts_as_multiframe_reply(void) {
  PollSched s;
  pollSchedInit(s, twoEntries(), 0);
  pollSchedSent(s, 0, 0);
  const uint8_t ff[8] = {0x10, 0x0A, 0x41, 12, 1, 2, 3, 4};
  TEST_ASSERT_EQUAL(POLL_RX_MULTIFRAME, pollSchedOnFrame(s, ff, 8, 7, nullptr, nullptr));
  TEST_ASSERT_EQUAL_UINT32(1, s.st[0].multiframe);
  TEST_ASSERT_EQUAL(-1, s.inflight);
}

void test_empty_plan_never_picks(void) {
  PollPlan p = {};
  PollSched s;
  pollSchedInit(s, p, 0);
  TEST_ASSERT_EQUAL(-1, pollSchedPick(s, 123456, 0));
}

void test_millis_wrap(void) {
  PollSched s;
  pollSchedInit(s, twoEntries(), 0xFFFFFF00u);
  pollSchedSent(s, 0, 0xFFFFFF00u);
  TEST_ASSERT_FALSE(pollSchedCheckTimeout(s, 0xFFFFFF50u, 100, 5000));
  TEST_ASSERT_TRUE(pollSchedCheckTimeout(s, 0x00000010u, 100, 5000));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_shared_vector_canonical_and_hash);
  RUN_TEST(test_carhub_plan_vector);
  RUN_TEST(test_empty_entries_is_a_valid_clear);
  RUN_TEST(test_malformed_json_is_400);
  RUN_TEST(test_contract_violations_are_422);
  RUN_TEST(test_boundaries_accepted);
  RUN_TEST(test_32_entries_ok_33_refused);
  RUN_TEST(test_nvs_encoding_round_trip_and_rejects);
  RUN_TEST(test_one_request_in_flight);
  RUN_TEST(test_reply_for_another_pid_is_ignored);
  RUN_TEST(test_cadence_from_send_time);
  RUN_TEST(test_most_overdue_goes_first);
  RUN_TEST(test_timeout_backs_off_and_caps);
  RUN_TEST(test_tx_refused_is_not_a_timeout);
  RUN_TEST(test_first_frame_counts_as_multiframe_reply);
  RUN_TEST(test_empty_plan_never_picks);
  RUN_TEST(test_millis_wrap);
  return UNITY_END();
}

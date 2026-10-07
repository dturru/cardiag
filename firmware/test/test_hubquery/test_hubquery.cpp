// hubquery.h: the async mDNS query lifecycle (timeout, cancel, a new query
// while one is pending) against a fake that enforces espressif/mdns 1.11.3's
// contract and records every misuse: a delete refused because the query is
// still running, a double free, a use after free, a second "done", a leak.

#include <unity.h>

#include "hubquery.h"

namespace {

enum QState { Q_FREE_SLOT = 0, Q_RUNNING, Q_DONE, Q_TAKEN, Q_DELETED };

struct FakeQuery {
  QState state;
  bool   hasResults;
  int    resultsLive;   // 1 while the results allocation exists
};

struct Fake {
  FakeQuery q[8];
  int  n;
  bool failStart;
  bool refuseDeleteOnce;   // a finished query whose delete is refused once
  // Misuse counters: all must stay 0.
  int  useAfterFree, doubleFree, doubleResultsFree, pollAfterDone;
  // Contract events (not misuse).
  int  deleteRefused, started, deleted, resultsFreed;
};

Fake F;

FakeQuery *Q(void *p) { return static_cast<FakeQuery *>(p); }

void *fStart(void *) {
  if (F.failStart) return nullptr;
  FakeQuery *q = &F.q[F.n++];
  *q = {Q_RUNNING, false, 0};
  F.started++;
  return q;
}

bool fPoll(void *, void *p, void **res) {
  FakeQuery *q = Q(p);
  if (q->state == Q_DELETED) { F.useAfterFree++; return false; }
  if (q->state == Q_TAKEN) { F.pollAfterDone++; return false; }  // semaphore gone
  if (q->state != Q_DONE) return false;
  q->state = Q_TAKEN;
  *res = q->hasResults ? q : nullptr;   // the results "pointer"
  return true;
}

void fFreeResults(void *, void *r) {
  if (!r) return;                        // mdns_query_results_free(NULL) is fine
  FakeQuery *q = Q(r);
  if (q->resultsLive == 0) { F.doubleResultsFree++; return; }
  q->resultsLive = 0;
  F.resultsFreed++;
}

bool fDestroy(void *, void *p) {
  FakeQuery *q = Q(p);
  if (q->state == Q_DELETED) { F.doubleFree++; return true; }
  if (q->state == Q_RUNNING) { F.deleteRefused++; return false; }  // INVALID_STATE
  if (F.refuseDeleteOnce) { F.refuseDeleteOnce = false; F.deleteRefused++; return false; }
  q->state = Q_DELETED;
  F.deleted++;
  return true;
}

// The component finishing a query: an answer (results) or a timeout (none).
void finish(void *p, bool answer) {
  FakeQuery *q = Q(p);
  q->state = Q_DONE;
  q->hasResults = answer;
  q->resultsLive = answer ? 1 : 0;
}

HubQueryApi api() { return {nullptr, fStart, fPoll, fFreeResults, fDestroy}; }

void assertNoMisuse() {
  TEST_ASSERT_EQUAL(0, F.useAfterFree);
  TEST_ASSERT_EQUAL(0, F.doubleFree);
  TEST_ASSERT_EQUAL(0, F.doubleResultsFree);
  TEST_ASSERT_EQUAL(0, F.pollAfterDone);
}

void assertNothingLeaked() {
  for (int i = 0; i < F.n; i++) {
    TEST_ASSERT_EQUAL_MESSAGE(Q_DELETED, F.q[i].state, "query not deleted");
    TEST_ASSERT_EQUAL_MESSAGE(0, F.q[i].resultsLive, "results not freed");
  }
}

int g_consumed;
void *g_seen;

}  // namespace

void setUp(void) { F = Fake{}; g_consumed = 0; g_seen = nullptr; }
void tearDown(void) {}

void test_answer_is_consumed_then_freed_and_deleted_once(void) {
  HubQuery h{};
  HubQueryApi a = api();
  TEST_ASSERT_TRUE(hubQueryStart(h, a));
  void *q = h.active;
  auto take = [](void *r) { g_consumed++; g_seen = r; };
  TEST_ASSERT_FALSE(hubQueryPoll(h, a, take));          // still running
  finish(q, true);
  TEST_ASSERT_TRUE(hubQueryPoll(h, a, take));
  TEST_ASSERT_EQUAL(1, g_consumed);
  TEST_ASSERT_EQUAL_PTR(q, g_seen);                     // results valid in consume
  TEST_ASSERT_FALSE(hubQueryPoll(h, a, take));          // never polled again
  TEST_ASSERT_FALSE(hubQueryBusy(h));
  TEST_ASSERT_EQUAL(1, F.deleted);
  TEST_ASSERT_EQUAL(1, F.resultsFreed);
  assertNoMisuse();
  assertNothingLeaked();
}

void test_timeout_without_results(void) {
  HubQuery h{};
  HubQueryApi a = api();
  hubQueryStart(h, a);
  finish(h.active, false);                              // HUB_MDNS_TIMEOUT_MS
  TEST_ASSERT_TRUE(hubQueryPoll(h, a, [](void *r) { g_seen = r; g_consumed++; }));
  TEST_ASSERT_NULL(g_seen);
  TEST_ASSERT_FALSE(hubQueryBusy(h));
  assertNoMisuse();
  assertNothingLeaked();
}

void test_cancel_while_running_parks_never_deletes_early(void) {
  HubQuery h{};
  HubQueryApi a = api();
  hubQueryStart(h, a);
  void *q = h.active;
  hubQueryCancel(h, a);                                 // link down
  TEST_ASSERT_NULL(h.active);
  TEST_ASSERT_EQUAL_PTR(q, h.parked);
  TEST_ASSERT_EQUAL(0, F.deleteRefused);                // did not even try
  TEST_ASSERT_FALSE(hubQueryReap(h, a));                // still running
  finish(q, true);                                      // the answer comes anyway
  TEST_ASSERT_TRUE(hubQueryReap(h, a));
  TEST_ASSERT_FALSE(hubQueryBusy(h));
  TEST_ASSERT_EQUAL(0, g_consumed);                     // nobody wanted it
  assertNoMisuse();
  assertNothingLeaked();
}

void test_new_query_while_one_is_parked_waits(void) {
  HubQuery h{};
  HubQueryApi a = api();
  hubQueryStart(h, a);
  void *first = h.active;
  hubQueryCancel(h, a);
  TEST_ASSERT_FALSE(hubQueryStart(h, a));               // link back up: refused
  TEST_ASSERT_EQUAL(1, F.started);
  finish(first, false);
  hubQueryReap(h, a);
  TEST_ASSERT_TRUE(hubQueryStart(h, a));                // now it may start
  finish(h.active, true);
  hubQueryPoll(h, a, [](void *) { g_consumed++; });
  TEST_ASSERT_EQUAL(1, g_consumed);
  assertNoMisuse();
  assertNothingLeaked();
}

void test_new_query_while_one_is_active_is_refused(void) {
  HubQuery h{};
  HubQueryApi a = api();
  hubQueryStart(h, a);
  void *q = h.active;
  TEST_ASSERT_FALSE(hubQueryStart(h, a));
  TEST_ASSERT_EQUAL_PTR(q, h.active);
  TEST_ASSERT_EQUAL(1, F.started);
  finish(q, false);
  hubQueryPoll(h, a, [](void *) {});
  assertNoMisuse();
  assertNothingLeaked();
}

void test_cancel_after_finish_retires_at_once(void) {
  HubQuery h{};
  HubQueryApi a = api();
  hubQueryStart(h, a);
  finish(h.active, true);                               // done, not yet polled
  hubQueryCancel(h, a);
  TEST_ASSERT_FALSE(hubQueryBusy(h));
  TEST_ASSERT_EQUAL(0, g_consumed);
  assertNoMisuse();
  assertNothingLeaked();
}

void test_cancel_twice_and_cancel_idle_are_harmless(void) {
  HubQuery h{};
  HubQueryApi a = api();
  hubQueryCancel(h, a);                                 // idle
  hubQueryStart(h, a);
  void *q = h.active;
  hubQueryCancel(h, a);
  hubQueryCancel(h, a);                                 // second link-down
  TEST_ASSERT_EQUAL_PTR(q, h.parked);
  finish(q, true);
  hubQueryReap(h, a);
  hubQueryReap(h, a);                                   // nothing left
  assertNoMisuse();
  assertNothingLeaked();
}

void test_start_failure_leaves_nothing_busy(void) {
  HubQuery h{};
  HubQueryApi a = api();
  F.failStart = true;
  TEST_ASSERT_FALSE(hubQueryStart(h, a));
  TEST_ASSERT_FALSE(hubQueryBusy(h));
  F.failStart = false;
  TEST_ASSERT_TRUE(hubQueryStart(h, a));
  finish(h.active, false);
  hubQueryPoll(h, a, [](void *) {});
  assertNoMisuse();
  assertNothingLeaked();
}

void test_refused_delete_after_finish_is_retried_not_repolled(void) {
  HubQuery h{};
  HubQueryApi a = api();
  hubQueryStart(h, a);
  finish(h.active, true);
  F.refuseDeleteOnce = true;
  hubQueryPoll(h, a, [](void *) {});
  TEST_ASSERT_TRUE(hubQueryBusy(h));                    // kept, not leaked
  TEST_ASSERT_FALSE(hubQueryStart(h, a));
  hubQueryReap(h, a);                                   // delete retried
  TEST_ASSERT_FALSE(hubQueryBusy(h));
  assertNoMisuse();                                     // incl. no poll after done
  assertNothingLeaked();
}

// The PR #29 code path, for the record: delete a RUNNING query and drop the
// pointer. The component refuses, and the query is never freed.
void test_old_linkdown_path_leaked(void) {
  HubQueryApi a = api();
  void *q = a.start(a.ctx);
  TEST_ASSERT_FALSE(a.destroy(a.ctx, q));               // ESP_ERR_INVALID_STATE
  q = nullptr;                                          // ...and forgotten
  finish(&F.q[0], true);
  TEST_ASSERT_EQUAL(Q_DONE, F.q[0].state);              // alive forever: a leak
  TEST_ASSERT_EQUAL(1, F.q[0].resultsLive);
}

// Many link flaps with queries in flight: still one search at a time, and
// everything freed exactly once in the end.
void test_link_flap_storm(void) {
  HubQuery h{};
  HubQueryApi a = api();
  for (int i = 0; i < 6; i++) {
    hubQueryReap(h, a);
    if (hubQueryStart(h, a)) {
      int live = 0;
      for (int k = 0; k < F.n; k++)
        live += (F.q[k].state == Q_RUNNING || F.q[k].state == Q_DONE);
      TEST_ASSERT_EQUAL(1, live);
    }
    void *q = h.active ? h.active : h.parked;
    hubQueryCancel(h, a);
    if (q && (i % 2)) finish(q, i % 3 == 0);
  }
  for (int k = 0; k < F.n; k++)
    if (F.q[k].state == Q_RUNNING) finish(&F.q[k], false);
  hubQueryReap(h, a);
  TEST_ASSERT_FALSE(hubQueryBusy(h));
  assertNoMisuse();
  assertNothingLeaked();
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_answer_is_consumed_then_freed_and_deleted_once);
  RUN_TEST(test_timeout_without_results);
  RUN_TEST(test_cancel_while_running_parks_never_deletes_early);
  RUN_TEST(test_new_query_while_one_is_parked_waits);
  RUN_TEST(test_new_query_while_one_is_active_is_refused);
  RUN_TEST(test_cancel_after_finish_retires_at_once);
  RUN_TEST(test_cancel_twice_and_cancel_idle_are_harmless);
  RUN_TEST(test_start_failure_leaves_nothing_busy);
  RUN_TEST(test_refused_delete_after_finish_is_retried_not_repolled);
  RUN_TEST(test_old_linkdown_path_leaked);
  RUN_TEST(test_link_flap_storm);
  return UNITY_END();
}

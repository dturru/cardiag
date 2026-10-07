#pragma once

// ---------------------------------------------------------------------------
// LIFECYCLE OF THE ONE ASYNCHRONOUS mDNS QUERY. Pure, no Arduino:
// test/test_hubquery drives it against a fake that enforces the contract.
//
// The contract is espressif/mdns 1.11.3's (the version arduino-esp32 3.3.11
// links; mdns_querier.c):
//   q = mdns_query_async_new(...)              NULL = could not start
//   mdns_query_async_get_results(q, 0, &r, &n) true ONCE, when finished; it
//                                              takes the done semaphore, so a
//                                              second call never returns true
//   mdns_query_results_free(r)                 r may be NULL (timeout)
//   mdns_query_async_delete(q)                 ESP_ERR_INVALID_STATE while the
//                                              query is still running, and
//                                              frees nothing then
//
// The bug this replaces (PR #29, hubResolveLinkDown): delete on a RUNNING
// query, return value ignored, pointer dropped. The component refused the
// delete, so every link drop with a query in flight leaked the search, its
// strings, its semaphore and any results. Now a cancelled query is PARKED and
// reaped once it finishes -- never deleted early, never dropped, never freed
// twice -- and no new query starts while one is parked, so at most one search
// exists at a time.
// ---------------------------------------------------------------------------

#include <stddef.h>

struct HubQueryApi {
  void *ctx;
  void *(*start)(void *ctx);                           // nullptr = failed
  bool  (*poll)(void *ctx, void *q, void **results);   // true = finished
  void  (*freeResults)(void *ctx, void *results);      // results may be null
  bool  (*destroy)(void *ctx, void *q);                // false = refused
};

struct HubQuery {
  void *active;     // the query whose answer we want
  void *parked;     // cancelled while running: reap when it finishes
  void *undeleted;  // finished and freed, but the delete was refused: retry
                    // the DELETE only (polling it again would never succeed:
                    // the done semaphore is already taken)
};

static inline bool hubQueryBusy(const HubQuery &h) {
  return h.active != nullptr || h.parked != nullptr || h.undeleted != nullptr;
}

// Finished: results freed, query deleted. 1.11.3 marks a query SEARCH_OFF
// before it signals done, so the delete succeeds; if it ever does not, the
// query is kept for a delete retry rather than leaked.
static inline void hubQueryRetire_(HubQuery &h, const HubQueryApi &a, void *q,
                                   void *results) {
  a.freeResults(a.ctx, results);
  if (!a.destroy(a.ctx, q)) h.undeleted = q;
}

// Start a query. False when one is running or parked, or the start failed.
static inline bool hubQueryStart(HubQuery &h, const HubQueryApi &a) {
  if (hubQueryBusy(h)) return false;
  h.active = a.start(a.ctx);
  return h.active != nullptr;
}

// Poll the active query. When it has finished, `consume(results)` runs (the
// results are valid only during that call) and the query is retired.
// Returns true when it finished on this call.
template <class F>
static inline bool hubQueryPoll(HubQuery &h, const HubQueryApi &a, F consume) {
  if (!h.active) return false;
  void *res = nullptr;
  if (!a.poll(a.ctx, h.active, &res)) return false;
  void *q = h.active;
  h.active = nullptr;
  consume(res);
  hubQueryRetire_(h, a, q, res);
  return true;
}

// Link down: we no longer want the answer. A query that already finished is
// retired now; one still running is parked.
static inline void hubQueryCancel(HubQuery &h, const HubQueryApi &a) {
  if (!h.active) return;
  void *q = h.active;
  h.active = nullptr;
  void *res = nullptr;
  if (a.poll(a.ctx, q, &res)) {
    hubQueryRetire_(h, a, q, res);
  } else {
    h.parked = q;   // empty: start refuses while anything is parked
  }
}

// Every loop pass, on or off the hub network. True when a parked query was
// reaped on this call.
static inline bool hubQueryReap(HubQuery &h, const HubQueryApi &a) {
  if (h.undeleted && a.destroy(a.ctx, h.undeleted)) h.undeleted = nullptr;
  if (!h.parked) return false;
  void *q = h.parked;
  void *res = nullptr;
  if (!a.poll(a.ctx, q, &res)) return false;
  h.parked = nullptr;
  hubQueryRetire_(h, a, q, res);
  return h.parked == nullptr;
}

#include <Arduino.h>
#include <ESPmDNS.h>      // links the mdns component; the raw IDF API is below
#include <mdns.h>
#include <esp_heap_caps.h>

#include "hubresolve.h"
#include "hubquery.h"
#include "creds.h"
#include "credstore.h"   // CRED_HUB_NAME_MAX
#include "config.h"
#include "session.h"

static HubResolveSched     g_sched;
static HubQuery            g_q = {};
static uint32_t            g_queryT0 = 0;
static HubAddrInputs       g_in = {};
static HubAddrChoice       g_choice = {0, HUBADDR_NONE};
static HubMdnsStats        g_st = {};
static bool                g_mdnsTried = false;
static uint32_t            g_linkUpMs = 0;
static uint32_t            g_lastSeenMs = 0;
static bool                g_up = false;
static char                g_name[CRED_HUB_NAME_MAX + 7];   // + ".local"
static char                g_self[CRED_LOGGER_NAME_MAX + 7];

// --- the mdns component's async query API, as hubquery.h's contract -------

static void *qStart(void *) {
  return mdns_query_async_new(credsHubName(), nullptr, nullptr, MDNS_TYPE_A,
                              HUB_MDNS_TIMEOUT_MS, 1, nullptr);
}
static bool qPoll(void *, void *q, void **res) {
  mdns_result_t *r = nullptr;
  uint8_t n = 0;
  // Zero timeout: done or not, never wait.
  if (!mdns_query_async_get_results((mdns_search_once_t *)q, 0, &r, &n)) return false;
  *res = r;
  return true;
}
static void qFreeResults(void *, void *r) { mdns_query_results_free((mdns_result_t *)r); }
static bool qDestroy(void *, void *q) {
  return mdns_query_async_delete((mdns_search_once_t *)q) == ESP_OK;
}
static const HubQueryApi kQueryApi = {nullptr, qStart, qPoll, qFreeResults, qDestroy};

// --- recent mDNS actions, for the heap check's failure line ----------------

enum MdnsAct : uint8_t {
  MA_NONE = 0, MA_INIT, MA_ADVERTISE, MA_QUERY, MA_QUERY_FAIL, MA_ANSWER,
  MA_NO_ANSWER, MA_CANCEL, MA_REAP,
};
static const char *mdnsActName(uint8_t a) {
  switch (a) {
    case MA_INIT: return "init"; case MA_ADVERTISE: return "advertise";
    case MA_QUERY: return "query"; case MA_QUERY_FAIL: return "query-fail";
    case MA_ANSWER: return "answer"; case MA_NO_ANSWER: return "no-answer";
    case MA_CANCEL: return "cancel"; case MA_REAP: return "reap";
    default: return "?";
  }
}
static struct { uint8_t act; uint32_t ms; } g_recent[4];
static uint8_t g_recentN = 0;
static void note(MdnsAct a) {
  g_recent[g_recentN % 4] = {(uint8_t)a, (uint32_t)millis()};
  g_recentN++;
}

void hubResolveDescribeRecent(char *out, size_t cap, uint32_t now) {
  size_t n = 0;
  out[0] = '\0';
  const uint8_t k = g_recentN < 4 ? g_recentN : 4;
  for (uint8_t i = 0; i < k && n < cap; i++) {
    const auto &r = g_recent[(uint8_t)(g_recentN - k + i) % 4];
    const int w = snprintf(out + n, cap - n, "%s%s+%lums", i ? " " : "",
                           mdnsActName(r.act), (unsigned long)(now - r.ms));
    if (w < 0) break;
    n += (size_t)w;
  }
  if (!k) snprintf(out, cap, "none");
}

static void ipStr(uint32_t ip, char out[16]) {
  snprintf(out, 16, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16 & 0xFF),
           (unsigned)(ip >> 8 & 0xFF), (unsigned)(ip & 0xFF));
}

static void reselect(const char *why) {
  const HubAddrChoice c = hubAddrSelect(g_in);
  if (c.ip == g_choice.ip && c.source == g_choice.source) return;
  g_choice = c;
  char s[16];
  ipStr(c.ip, s);
  Serial.printf("[hub] address %s (%s) -- %s\n", s, hubAddrSourceName(c.source), why);
}

// Register <logger_name>.local and _cardiag._tcp (TXT device_id, decimal as
// in /api/v1/session) on the responder mdns_init() started.
static bool advertiseSelf() {
#if !CARDIAG_MDNS_ADVERTISE
  return false;
#endif
  note(MA_ADVERTISE);
  snprintf(g_self, sizeof(g_self), "%s.local", credsLoggerName());
  if (mdns_hostname_set(credsLoggerName()) != ESP_OK) return false;
  mdns_instance_name_set(credsLoggerName());
  char id[12];
  snprintf(id, sizeof(id), "%lu", (unsigned long)sessionDeviceId());
  mdns_txt_item_t txt[] = {{"device_id", id}};
  return mdns_service_add(nullptr, LOGGER_MDNS_SERVICE, LOGGER_MDNS_PROTO,
                          WEB_HTTP_PORT, txt, 1) == ESP_OK;
}

// The mdns component starts its own task and allocates its state on the
// heap. Measured once, here, around init AND the advertisement, so the cost
// is a number rather than a guess.
static void mdnsInitOnce() {
  if (g_mdnsTried) return;
  g_mdnsTried = true;
#if !CARDIAG_MDNS_RESOLVE && !CARDIAG_MDNS_ADVERTISE
  Serial.println("[mdns] OFF in this build (CARDIAG_MDNS_RESOLVE=0, "
                 "CARDIAG_MDNS_ADVERTISE=0): mdns_init() not called");
  return;
#endif
  note(MA_INIT);
  g_st.heapBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  g_st.largestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  const esp_err_t e = mdns_init();
  g_st.initOk = (e == ESP_OK || e == ESP_ERR_INVALID_STATE);   // already up = fine
  if (g_st.initOk) g_st.advertised = advertiseSelf();
  g_st.heapAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  g_st.largestAfter = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  if (CARDIAG_MDNS_ADVERTISE)
    Serial.printf("[mdns] advertising %s + %s.%s port %u: %s\n",
                  g_self[0] ? g_self : "?", LOGGER_MDNS_SERVICE, LOGGER_MDNS_PROTO,
                  (unsigned)WEB_HTTP_PORT, g_st.advertised ? "ok" : "FAILED");
  else
    Serial.println("[mdns] advertising OFF in this build (CARDIAG_MDNS_ADVERTISE=0)");
  Serial.printf("[mdns] init %s: internal heap %lu -> %lu (%ld B), largest "
                "block %lu -> %lu\n", g_st.initOk ? "ok" : "FAILED",
                (unsigned long)g_st.heapBefore, (unsigned long)g_st.heapAfter,
                (long)g_st.heapAfter - (long)g_st.heapBefore,
                (unsigned long)g_st.largestBefore, (unsigned long)g_st.largestAfter);
}

void hubResolveLinkUp(uint32_t gateway, uint32_t nvs) {
  snprintf(g_name, sizeof(g_name), "%s.local", credsHubName());
  g_in.gateway = gateway;
  g_in.nvs = nvs;
  g_in.mdns = 0;                 // a new session: no fresh answer yet
  g_up = true;
  g_linkUpMs = millis();
  reselect("link up");
  mdnsInitOnce();
  if (!g_st.initOk || !CARDIAG_MDNS_RESOLVE) return;   // nvs / gateway only
  hubResolveReset(g_sched);
  hubResolveKick(g_sched, millis(), HUB_MDNS_MIN_GAP_MS);
}

void hubResolveLinkDown() {
  g_up = false;
  // NOT mdns_query_async_delete() on a running query: the component refuses
  // it (ESP_ERR_INVALID_STATE) and the dropped pointer leaked the search.
  if (g_q.active) {
    g_st.parked++;
    note(MA_CANCEL);
    hubQueryCancel(g_q, kQueryApi);
  }
  hubResolveReset(g_sched);
}

void hubResolveReap() {
  if (hubQueryReap(g_q, kQueryApi)) {
    g_st.reaped++;
    note(MA_REAP);
  }
}

static uint32_t firstV4(const mdns_result_t *r) {
  for (; r; r = r->next) {
    for (const mdns_ip_addr_t *a = r->addr; a; a = a->next) {
      if (a->addr.type != ESP_IPADDR_TYPE_V4) continue;
      const uint8_t *b = (const uint8_t *)&a->addr.u_addr.ip4.addr;  // network order
      const uint32_t ip = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
                          (uint32_t)b[2] << 8 | b[3];
      if (hubAddrUsable(ip)) return ip;
    }
  }
  return 0;
}

void hubResolveLoop() {
  if (!g_up || !g_st.initOk || !CARDIAG_MDNS_RESOLVE) return;
  const uint32_t now = millis();

  if (g_q.active) {
    uint32_t ip = 0;
    // The results are valid only inside the callback; hubquery.h frees them
    // and deletes the query right after.
    if (!hubQueryPoll(g_q, kQueryApi,
                      [&ip](void *r) { ip = firstV4((const mdns_result_t *)r); }))
      return;
    note(ip ? MA_ANSWER : MA_NO_ANSWER);
    g_st.lastQueryMs = now - g_queryT0;
    hubResolveFinished(g_sched, now, ip != 0, HUB_MDNS_BACKOFF_BASE_MS,
                       HUB_MDNS_BACKOFF_MAX_MS);
    if (ip) {
      g_st.answers++;
      g_st.lastAnswerIp = ip;
      g_in.mdns = ip;
      g_in.mdnsCache = ip;
      g_lastSeenMs = now;        // a fresh answer: give it a full silence window
      reselect("mDNS answer");
    } else {
      g_st.failures++;
      g_in.mdns = 0;
      char why[64];
      snprintf(why, sizeof(why), "no mDNS answer for %s (retry in %lus)", g_name,
               (unsigned long)((g_sched.nextMs - now) / 1000));
      reselect(why);
      if (g_st.failures == 1 || g_sched.fails <= 1)
        Serial.printf("[hub] %s\n", why);
    }
    return;
  }

  if (hubSilent(now, g_lastSeenMs, g_linkUpMs, HUB_SILENT_MS))
    hubResolveKick(g_sched, now, HUB_MDNS_MIN_GAP_MS);

  if (!hubResolveDue(g_sched, now)) return;
  if (hubQueryBusy(g_q)) return;  // a cancelled query not reaped yet: wait
  const bool started = hubQueryStart(g_q, kQueryApi);
  hubResolveStarted(g_sched, now);
  g_queryT0 = now;
  g_st.queries++;
  note(started ? MA_QUERY : MA_QUERY_FAIL);
  if (!started) {                // could not even start: counts as a failure
    g_st.failures++;
    hubResolveFinished(g_sched, now, false, HUB_MDNS_BACKOFF_BASE_MS,
                       HUB_MDNS_BACKOFF_MAX_MS);
  }
}

void hubResolveNoteRequest(uint32_t remote) {
  if (!g_up || !remote) return;
  if (remote == g_choice.ip) {
    g_lastSeenMs = millis();
    return;
  }
  // Not from where we send UDP: maybe the hub on a new address -- or just a
  // laptop on the bench polling /api/v1. Only a hint once the current hub
  // address has missed a poll, so a bench tool cannot drive a query storm.
  const uint32_t now = millis();
  if (g_st.initOk && hubSilent(now, g_lastSeenMs, g_linkUpMs, HUB_SILENT_MS / 2))
    hubResolveKick(g_sched, now, HUB_MDNS_MIN_GAP_MS);
}

uint32_t      hubResolveIp() { return g_choice.ip; }
HubAddrSource hubResolveSource() { return g_choice.source; }
const char   *hubResolveName() { return g_name; }
const char   *hubResolveSelfName() { return g_self; }
const HubMdnsStats *hubResolveStats() { return &g_st; }

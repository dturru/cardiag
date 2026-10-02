#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "poller.h"
#include "cantx.h"
#include "obd.h"
#include "config.h"
#include "bookend.h"
#include "filestore.h"
#include "power.h"
#include "session.h"

static_assert(IGN_OFF_DEBOUNCE_MS > BOOKEND_END_MARGIN_MS + BOOKEND_MIN_START_MS,
              "the ignition OFF debounce must leave room for a trip-end read");

static const char kNs[]  = "pollplan";
static const char kKey[] = "plan";

struct PollFrame {
  uint32_t id;
  uint8_t dlc;
  uint8_t data[8];
};

static PollPlan      g_plan = {};
static bool          g_hasPlan = false;
static PollSched     g_sched;
static bool          g_active = false;
static QueueHandle_t g_q = nullptr;
static volatile uint32_t g_rxDropped = 0;

// Tier C bookends share the one-request-in-flight rule with the plan: while a
// bookend runs, the plan scheduler waits.
static BookendSeq g_bk;
static bool       g_bkActive = false;
static bool       g_tripStarted = false;   // START written this ignition cycle
static bool       g_endTried = false;      // END attempted for this OFF
static const BookendTiming kBkTiming = {BOOKEND_QUIET_MS, BOOKEND_REQ_MAX_MS,
                                        BOOKEND_MIN_START_MS};

static void loadNvs() {
  Preferences p;
  if (!p.begin(kNs, true)) return;           // namespace absent = no plan
  uint8_t blob[POLLPLAN_BLOB_MAX];
  const size_t n = p.isKey(kKey) ? p.getBytesLength(kKey) : 0;
  if (n && n <= sizeof(blob) && p.getBytes(kKey, blob, n) == n) {
    if (pollPlanDecode(blob, n, &g_plan) && g_plan.n > 0) {
      g_hasPlan = true;
    } else {
      memset(&g_plan, 0, sizeof(g_plan));
      Serial.println("[poll] stored plan fails validation -- ignored");
    }
  }
  p.end();
}

static bool saveNvs(const PollPlan &plan) {
  Preferences p;
  if (!p.begin(kNs, false)) return false;
  bool ok;
  if (plan.n == 0) {
    ok = !p.isKey(kKey) || p.remove(kKey);
  } else {
    uint8_t blob[POLLPLAN_BLOB_MAX];
    const size_t n = pollPlanEncode(plan, blob, sizeof(blob));
    ok = n && p.putBytes(kKey, blob, n) == n;
  }
  p.end();
  return ok;
}

void pollerBegin() {
  if (!g_q) g_q = xQueueCreate(POLL_RX_QUEUE_LEN, sizeof(PollFrame));
  loadNvs();
  if (g_hasPlan) {
    char h[POLL_HASH_HEX + 1];
    pollPlanHash(g_plan, h);
    Serial.printf("[poll] plan from NVS: %u entries, hash %s\n",
                  (unsigned)g_plan.n, h);
  } else {
    Serial.println("[poll] no poll plan stored (POST /api/v1/pollplan)");
  }
}

void pollerEnter() {
  if (g_q) xQueueReset(g_q);
  pollSchedInit(g_sched, g_plan, millis());
  g_active = true;
  if (!g_hasPlan) {
    Serial.println("[poll] MODE_POLL IDLE: no poll plan -- nothing will be "
                   "transmitted. The hub sets one with POST /api/v1/pollplan.");
    return;
  }
  Serial.printf("[poll] running plan: %u entries\n", (unsigned)g_plan.n);
  for (uint8_t i = 0; i < g_plan.n; i++)
    Serial.printf("[poll]   mode %02X pid %02X every %lu ms\n",
                  g_plan.e[i].mode, g_plan.e[i].pid,
                  (unsigned long)g_plan.e[i].periodMs);
}

static const char *kindName(uint8_t k) {
  return k == BOOKEND_KIND_START ? "trip_start" : "trip_end";
}

// The sequence is done: write the record, or say exactly why not.
static void bookendWrite() {
  g_bkActive = false;
  const char *why = nullptr;
  if (!bookendSeqShouldWrite(g_bk, &why)) {
    Serial.printf("[bookend] %s SKIPPED: %s\n", kindName(g_bk.d.kind), why);
    return;
  }
  static uint8_t rec[BOOKEND_REC_MAX];
  const size_t n = bookendEncode(g_bk.d, sessionDeviceId(), sessionBootId(),
                                 millis(), rec, sizeof(rec));
  const bool ok = n && filestoreWriteBookend(rec, n);
  Serial.printf("[bookend] %s %s: flags 0x%02X stored %u pending %u m06 %u "
                "(%u B)\n", kindName(g_bk.d.kind), ok ? "written" : "WRITE FAILED",
                g_bk.d.flags, g_bk.d.nStored, g_bk.d.nPending, g_bk.d.nM06,
                (unsigned)n);
}

static void bookendStart(uint8_t kind, uint32_t now, uint32_t budgetMs) {
  bookendSeqStart(g_bk, kind, now, budgetMs, kBkTiming);
  g_bkActive = true;
  Serial.printf("[bookend] %s: reading 01/01, 03, 07, 06 (budget %lu ms)\n",
                kindName(kind), (unsigned long)budgetMs);
}

// Trip boundaries. START: ignition on and the plan has had an answer. END:
// the ignition input reads OFF and the debounce has not confirmed it -- the
// gate is still open for exactly that long.
static void bookendTriggers(uint32_t now) {
  uint32_t edge = 0;
  const bool offPending = powerIgnitionOffPending(&edge);

  if (g_bkActive) {
    // A dip that recovers (cranking) is not a key-off: drop the END quietly.
    if (g_bk.d.kind == BOOKEND_KIND_END && !offPending && powerIgnitionOn()) {
      g_bkActive = false;
      g_endTried = false;
      Serial.println("[bookend] trip_end discarded: ignition back on");
    }
    return;
  }
  if (!powerIgnitionOn()) {               // confirmed OFF: next ON is a new trip
    g_tripStarted = false;
    g_endTried = false;
    return;
  }
  if (!offPending) g_endTried = false;
  if (g_sched.inflight >= 0) return;      // one request in flight, always

  if (!g_tripStarted && g_sched.anyOk && !offPending) {
    g_tripStarted = true;
    bookendStart(BOOKEND_KIND_START, now, BOOKEND_START_BUDGET_MS);
    return;
  }
  if (g_tripStarted && offPending && !g_endTried) {
    g_endTried = true;
    const int32_t left = (int32_t)(edge + IGN_OFF_DEBOUNCE_MS -
                                   BOOKEND_END_MARGIN_MS - now);
    if (left < (int32_t)BOOKEND_MIN_START_MS) {
      Serial.printf("[bookend] trip_end SKIPPED: %ld ms left of the ignition "
                    "OFF window, need %u\n", (long)left,
                    (unsigned)BOOKEND_MIN_START_MS);
      return;
    }
    bookendStart(BOOKEND_KIND_END, now, (uint32_t)left);
  }
}

static void bookendTick(uint32_t now) {
  BookendTx tx;
  for (int k = 0; k < 4 && g_bkActive; k++) {     // FC + request in one pass
    const BookendAct a = bookendSeqPoll(g_bk, now, &tx);
    if (a == BK_DONE) { bookendWrite(); return; }
    if (a == BK_WAIT) return;
    twai_message_t m = {};
    m.identifier = tx.id;
    m.data_length_code = 8;
    memcpy(m.data, tx.data, 8);
    // Through the gate, zero ticks: a refusal ends the sequence, never waits.
    if (canTransmit(&m, 0) != ESP_OK) bookendSeqTxFailed(g_bk);
  }
}

void pollerExit() {
  // Leaving POLL mid-bookend (the confirmed OFF drops to LISTEN): keep what
  // was read, under the same write-or-skip rule.
  if (g_bkActive) {
    bookendSeqTxFailed(g_bk);
    bookendWrite();
  }
  g_active = false;
}

void pollerNoteFrame(const twai_message_t &rx) {
  if (!g_active || !g_q || rx.extd) return;
  if (rx.identifier < OBD_RESP_ID_FIRST || rx.identifier > OBD_RESP_ID_LAST) return;
  PollFrame f;
  f.id = rx.identifier;
  f.dlc = rx.data_length_code > 8 ? 8 : rx.data_length_code;
  memcpy(f.data, rx.data, 8);
  if (xQueueSend(g_q, &f, 0) != pdTRUE) g_rxDropped++;
}

void pollerTick() {
  if (!g_active) return;
  const uint32_t now = millis();

  PollFrame f;
  while (g_q && xQueueReceive(g_q, &f, 0) == pdTRUE) {
    if (g_bkActive) bookendSeqOnFrame(g_bk, f.id, f.data, f.dlc, now);
    else pollSchedOnFrame(g_sched, f.data, f.dlc, now, nullptr, nullptr);
  }

  if (g_hasPlan) bookendTriggers(now);
  if (g_bkActive) { bookendTick(now); return; }

  pollSchedCheckTimeout(g_sched, now, OBD_RESPONSE_TIMEOUT_MS,
                        POLL_BACKOFF_MAX_MS);

  const int i = pollSchedPick(g_sched, now, OBD_INTER_REQUEST_MS);
  if (i < 0) return;

  twai_message_t tx = {};
  tx.identifier       = OBD_REQ_ID_FUNCTIONAL;
  tx.data_length_code = 8;
  tx.data[0] = 0x02;
  tx.data[1] = g_sched.plan.e[i].mode;
  tx.data[2] = g_sched.plan.e[i].pid;
  tx.data[3] = tx.data[4] = tx.data[5] = tx.data[6] = tx.data[7] = 0x55;

  // Zero ticks: a full TX queue is a failed send, never a wait. The gate
  // (cantx.h) refuses while the ignition is off.
  if (canTransmit(&tx, 0) == ESP_OK) pollSchedSent(g_sched, i, now);
  else pollSchedTxFailed(g_sched, i, now);
}

PollPlanResult pollerSetFromJson(const char *json, size_t n,
                                 char hashOut[POLL_HASH_HEX + 1]) {
  PollPlan p;
  PollPlanResult r = pollPlanParse(json, n, &p);
  if (r.status != POLLPLAN_OK) return r;
  if (!saveNvs(p)) {
    r.status = POLLPLAN_INVALID;
    r.error = "NVS write failed";
    return r;
  }
  g_plan = p;
  g_hasPlan = p.n > 0;
  pollPlanHash(p, hashOut);
  Serial.printf("[poll] plan %s: %u entries, hash %s\n",
                g_hasPlan ? "set" : "CLEARED", (unsigned)p.n, hashOut);
  if (g_active) pollerEnter();               // apply now
  return r;
}

bool    pollerHasPlan() { return g_hasPlan; }
uint8_t pollerEntries() { return g_hasPlan ? g_plan.n : 0; }
void    pollerHash(char out[POLL_HASH_HEX + 1]) { pollPlanHash(g_plan, out); }
bool    pollerAnyReply() { return g_active && g_sched.anyOk; }
uint32_t pollerRxDropped() { return g_rxDropped; }

void pollerPrintStats() {
  if (!g_hasPlan) {
    Serial.println("-- MODE_POLL idle: no poll plan");
    return;
  }
  Serial.printf("-- poll: rx dropped %lu | tx blocked %lu\n",
                (unsigned long)g_rxDropped, (unsigned long)canTxBlocked());
  for (uint8_t i = 0; i < g_sched.plan.n; i++) {
    const PollPidStats &s = g_sched.st[i];
    Serial.printf("   %02X/%02X req %lu ok %lu to %lu mf %lu txf %lu "
                  "lat %lums miss %u\n",
                  g_sched.plan.e[i].mode, g_sched.plan.e[i].pid,
                  (unsigned long)s.requests, (unsigned long)s.replies,
                  (unsigned long)s.timeouts, (unsigned long)s.multiframe,
                  (unsigned long)s.txFailed, (unsigned long)s.lastLatencyMs,
                  (unsigned)s.misses);
  }
}

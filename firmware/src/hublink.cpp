#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "hublink.h"
#include "webui.h"
#include "looptime.h"
#include "candrops.h"
#include "filestore.h"
#include "retrybackoff.h"
#include "config.h"
#include "secrets.h"

static HubLinkState g_state    = HUBLINK_OFF;
static uint32_t     g_lastTry  = 0;
static uint32_t     g_retryMs  = WIFI_STA_RETRY_MS;
// The wait between attempts to find the hub. Reset on every STA disconnect,
// advanced after every failed join (retrybackoff.h).
static RetryBackoff g_backoff;
static uint32_t     g_linkUpMs = 0;
static IPAddress    g_hubIp;
static HubLinkStats g_stats;

// Written by the Wi-Fi task, read by loop(). Only ever set in the event
// handler and cleared in hublinkLoop(), so a plain volatile flag is enough --
// no queue, no lock, and nothing in the event context that can block.
static volatile bool    g_dropFlag    = false;
static volatile uint8_t g_dropReason  = 0;
static volatile uint32_t g_dropMs     = 0;

// Set around every deliberate teardown. WiFi.disconnect() and a mode change
// both raise STA_DISCONNECTED, and treating our own teardown as a fault would
// make the board fall back to AP from inside the code that is already doing it.
static volatile bool g_teardown = false;

// NEVER WIFI_AP_STA.
//
// The ESP32 has ONE radio. In AP+STA the two interfaces must share a channel,
// so joining a hub AP on channel 6 silently drags this board's own AP off
// WIFI_AP_CHANNEL. Clients that were told to expect channel 1 then fail in a
// way that looks like a range problem. Run one mode at a time.

// ---------------------------------------------------------------------------
// Why an EVENT and not a poll.
//
// The old loop polled WiFi.status() and, on losing it, waited out the 60 s
// re-join timer before doing anything -- about 2.5 minutes from "the hub went
// away" to "the board is serving its own UI again". In a car that is the whole
// window in which there is no interface at all.
//
// STA_DISCONNECTED fires from the Wi-Fi task as soon as the driver gives up on
// the AP, which for a hub that simply vanished is the beacon timeout, a few
// seconds. That is the floor: the radio cannot know the AP is gone before it
// has missed enough beacons to be sure. The poll is KEPT as a backstop for the
// case where the link dies without an event (it should not, but "should not"
// is not a detection strategy), and the two are counted separately so the soak
// test can show which one is actually doing the work.
// ---------------------------------------------------------------------------

static void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;
  if (g_teardown) return;              // our own doing; not a fault
  if (g_state != HUBLINK_STA) return;  // not up yet, or already fallen back

  g_dropReason = info.wifi_sta_disconnected.reason;
  g_dropMs     = millis();
  g_dropFlag   = true;                 // set LAST: loop() reads it as the gate
}

void hublinkPrintStats(const char *what) {
  const HubLinkStats &s = g_stats;
  const LoopStats win = cardiagLoopTakeWindow(LOOP_WIN_LOG);
  const FsSubWindow fsw = filestoreTakeSubWindow(1);
  const CanDrops drops = cardiagCanDrops();
  const char *bootStage = cardiagLoopStats()->maxStage;
  Serial.printf("[hublink] %s state=%s joins=%lu drops=%lu(evt=%lu poll=%lu) "
                "joinfail=%lu apstarts=%lu reason=%u fallback=%lums "
                // largest= is the fragmentation half of the story. Free heap
                // can look fine while no single block is big enough to serve
                // a request, and the soak logs it per cycle for exactly that
                // reason: a flat `heap` with a falling `largest` is still a
                // board on its way to a failed allocation.
                "worst=%lums heap=%lu minheap=%lu largest=%lu "
                // Max loop() pass since boot, and its slowest stage. The
                // non-blocking join exists to keep this small.
                "loopmax=%luus loopstage=%s "
                // Max pass since the PREVIOUS stats line, which this line
                // resets. The soak takes the max of these per cycle, so a
                // cycle's loop max is that cycle's, not the whole boot's.
                "loopwin=%luus loopwinstage=%s "
                // Worst filestore sub-stage (fsprof.h) since the previous
                // stats line, and the whole tick in that pass.
                "fswin=%luus fswinstage=%s fswinpass=%luus "
                // Frames lost before the file (candrops.h). Any non-zero
                // fails the soak.
                "canmiss=%lu canovr=%lu chgdrop=%lu idovf=%lu\n",
                what, hublinkStateName(),
                (unsigned long)s.staJoins, (unsigned long)s.staDrops,
                (unsigned long)s.eventDrops, (unsigned long)s.pollDrops,
                (unsigned long)s.joinFailures, (unsigned long)s.apStarts,
                (unsigned)s.lastReason,
                (unsigned long)s.lastFallbackMs,
                (unsigned long)s.worstFallbackMs,
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getMinFreeHeap(),
                (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned long)cardiagLoopStats()->maxUs,
                (bootStage && *bootStage) ? bootStage : "-",
                (unsigned long)win.maxUs,
                (win.maxStage && *win.maxStage) ? win.maxStage : "-",
                (unsigned long)fsw.worstUs,
                fsw.worstUs ? fsSubName(fsw.worstSub) : "-",
                (unsigned long)fsw.passUs,
                (unsigned long)drops.rxMissed, (unsigned long)drops.rxOverrun,
                (unsigned long)drops.changelogDropped,
                (unsigned long)drops.idOverflow);
}

// ---------------------------------------------------------------------------
// THE JOIN IS A STATE MACHINE, ONE STEP PER loop() PASS.
//
// 🐛 WHY (2026-09-25). tryJoin() used to sit in `while (...) delay(50)` for up
// to WIFI_STA_TIMEOUT_MS (8 s), and a FAILED join then ran four Wi-Fi driver
// mode switches (AP off, STA on, STA off, AP on) in the same loop() pass.
// Two things came of it:
//   * In SELFTEST the board's own loopback traffic stopped for the duration,
//     the filestore read >= 3 s of silence as key-off and closed every file:
//     686 ~5 KB files over the 200-cycle soak.
//   * The cycle-135 TASK_WDT followed a failed rejoin; the review
//     (docs/reviews/2026-09-25-rejoin-path-wdt.md) found that chain the prime
//     suspect -- 8 s bounded by this code, four core calls bounded by nothing
//     here, all in one unfed pass.
//
// Now each phase below does at most ONE driver mode switch and never waits:
// it does its step and returns, and the next loop() pass does the next one.
// The join timeout is measured across passes. ⚠️ How long each core call
// (WiFi.mode(), softAP()) takes by itself is the core's business and cannot
// be bounded from here -- which is what the loop-latency metric
// (looptime.h, /api/v1/session "loop") is for.
// ---------------------------------------------------------------------------

enum JoinPhase : uint8_t {
  PH_IDLE = 0,     // steady: serving the AP, or STA up
  PH_JOIN_RADIO,   // stop the server; WiFi.mode(STA)           [1 mode switch]
  PH_JOIN_BEGIN,   // WiFi.begin()                              [no switch]
  PH_JOIN_WAIT,    // poll WiFi.status() until up or timeout    [no switch]
  PH_LEAVE_STA,    // stop the server; WiFi.disconnect(false)   [no switch]
  PH_AP_RADIO,     // WiFi.mode(AP)                             [1 mode switch]
  PH_AP_SERVE,     // softAP() config + server begin            [no switch]
};

static JoinPhase g_phase = PH_IDLE;
static uint32_t  g_joinT0 = 0;
static bool      g_bootJoin = false;       // for the stats label only
static uint32_t  g_fallbackNoticedMs = 0;  // 0 = this AP start is not a fallback

static void startJoin(bool atBoot) {
  Serial.printf("[hublink] joining \"%s\" ...\n", WIFI_STA_SSID);
  g_bootJoin = atBoot;
  g_teardown = true;                 // our own mode changes raise events
  g_phase = PH_JOIN_RADIO;
}

// Drop out of STA and serve the board's own AP again. `viaEvent` only affects
// accounting -- the action is identical either way, which is the point: the
// event is a faster trigger for a path that already worked.
static void fallBackToAp(bool viaEvent, uint32_t noticedMs) {
  g_stats.staDrops++;
  if (viaEvent) g_stats.eventDrops++; else g_stats.pollDrops++;
  g_stats.lastReason  = g_dropReason;
  g_stats.lastDropMs  = noticedMs;
  g_stats.lastLinkUpMs = g_linkUpMs ? (noticedMs - g_linkUpMs) : 0;

  Serial.printf("[hublink] STA lost (%s, reason=%u); falling back to AP\n",
                viaEvent ? "event" : "poll", (unsigned)g_dropReason);

  g_teardown = true;
  g_state = HUBLINK_AP;              // not on the hub any more, from now
  g_fallbackNoticedMs = noticedMs;
  g_linkUpMs = 0;
  g_phase = PH_LEAVE_STA;
}

// One step. Returns having done at most one driver mode switch.
static void stepJoin() {
  switch (g_phase) {
    case PH_IDLE:
      return;

    case PH_JOIN_RADIO:
      webuiServerStop();             // the socket; routes stay registered
      // AP -> STA in ONE mode switch. The old path went AP -> OFF -> STA.
      WiFi.mode(WIFI_STA);
      // We own the fallback policy; the core's own reconnect would race it.
      WiFi.setAutoReconnect(false);
      g_phase = PH_JOIN_BEGIN;
      return;

    case PH_JOIN_BEGIN:
      WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);   // asynchronous
      g_dropFlag = false;
      g_joinT0 = millis();
      g_phase = PH_JOIN_WAIT;
      return;

    case PH_JOIN_WAIT:
      if (WiFi.status() == WL_CONNECTED) {
        g_hubIp    = WiFi.gatewayIP();
        g_linkUpMs = millis();
        g_dropFlag = false;          // drop the join-phase noise
        g_teardown = false;
        g_stats.staJoins++;
        g_state = HUBLINK_STA;
        g_phase = PH_IDLE;
        Serial.printf("[hublink] STA up: ip=%s gw=%s rssi=%d\n",
                      WiFi.localIP().toString().c_str(),
                      g_hubIp.toString().c_str(), WiFi.RSSI());
        webuiStartOnCurrentNetwork();   // same routes, no AP
        hublinkPrintStats(g_bootJoin ? "boot-sta" : "rejoin");
        return;
      }
      if (millis() - g_joinT0 < WIFI_STA_TIMEOUT_MS) return;   // not yet
      Serial.println("[hublink] hub network not found");
      g_stats.joinFailures++;
      g_fallbackNoticedMs = 0;       // a failed join is not a fallback
      g_phase = PH_LEAVE_STA;
      return;

    case PH_LEAVE_STA:
      webuiServerStop();
      // wifioff=false: disconnect WITHOUT a mode switch. The switch to AP is
      // the next pass's one.
      WiFi.disconnect(false);
      g_phase = PH_AP_RADIO;
      return;

    case PH_AP_RADIO:
      WiFi.mode(WIFI_AP);
      g_phase = PH_AP_SERVE;
      return;

    case PH_AP_SERVE: {
      const bool up = webuiServeAp();   // softAP() config + server begin
      g_state = HUBLINK_AP;
      g_teardown = false;
      g_dropFlag = false;
      g_phase = PH_IDLE;
      if (up) g_stats.apStarts++;
      if (g_fallbackNoticedMs) {
        const uint32_t took = millis() - g_fallbackNoticedMs;
        g_stats.lastFallbackMs = took;
        if (took > g_stats.worstFallbackMs) g_stats.worstFallbackMs = took;
        g_fallbackNoticedMs = 0;
        // A drop: the schedule starts again at its shortest wait.
        backoffReset(&g_backoff);
        g_retryMs = backoffNextMs(&g_backoff);
        hublinkPrintStats("fallback");
      } else {
        // A join that failed: wait longer next time, up to the cap.
        g_retryMs = backoffNextMs(&g_backoff);
        if (g_bootJoin) hublinkPrintStats("boot-ap");
      }
      g_bootJoin = false;
      g_lastTry = millis();
      return;
    }
  }
}

void hublinkBegin() {
  memset(&g_stats, 0, sizeof(g_stats));
  WiFi.onEvent(onWifiEvent);
  g_lastTry = millis();
  g_retryMs = WIFI_STA_RETRY_MS;
  backoffInit(&g_backoff, WIFI_STA_BACKOFF_FIRST_MS, WIFI_STA_RETRY_MS);
  // Starts the machine and RETURNS. setup() no longer waits up to 8 s here;
  // loop() drives the join, and on failure the board falls back to exactly
  // what it did before the hub existed: its own AP. The logger is a
  // standalone product; the hub is an optional client.
  g_state = HUBLINK_AP;
  startJoin(/*atBoot=*/true);
}

void hublinkLoop() {
  if (g_phase != PH_IDLE) {
    stepJoin();
    return;
  }

  if (g_state == HUBLINK_STA) {
    // Fast path: the driver already told us, seconds ago.
    if (g_dropFlag) {
      const uint32_t noticed = g_dropMs;
      g_dropFlag = false;
      fallBackToAp(/*viaEvent=*/true, noticed);
      return;
    }
    // Backstop. If this ever fires, the event path missed something and the
    // counter says so rather than the failure being invisible.
    if (WiFi.status() != WL_CONNECTED) {
      fallBackToAp(/*viaEvent=*/false, millis());
    }
    return;
  }

  if (g_state != HUBLINK_AP) return;

  // Periodically look for the hub again, so the logger joins when the car gets
  // home without needing a power cycle.
  if (millis() - g_lastTry < g_retryMs) return;
  g_lastTry = millis();
  startJoin(/*atBoot=*/false);
}

HubLinkState hublinkState() { return g_state; }

const char *hublinkStateName() {
  // Mid-transition is its own answer: neither the AP nor the hub link is
  // serving while the machine steps between them.
  if (g_phase != PH_IDLE) return "switching";
  switch (g_state) {
    case HUBLINK_STA: return "sta";
    case HUBLINK_AP:  return "ap";
    default:          return "off";
  }
}

bool      hublinkOnHub() { return g_state == HUBLINK_STA && g_phase == PH_IDLE; }
IPAddress hublinkHubIp() { return g_hubIp; }

const HubLinkStats *hublinkStats() { return &g_stats; }

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "hublink.h"
#include "webui.h"
#include "config.h"
#include "secrets.h"

static HubLinkState g_state    = HUBLINK_OFF;
static uint32_t     g_lastTry  = 0;
static uint32_t     g_retryMs  = WIFI_STA_RETRY_MS;
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
  Serial.printf("[hublink] %s state=%s joins=%lu drops=%lu(evt=%lu poll=%lu) "
                "joinfail=%lu apstarts=%lu reason=%u fallback=%lums "
                "worst=%lums heap=%lu minheap=%lu\n",
                what, hublinkStateName(),
                (unsigned long)s.staJoins, (unsigned long)s.staDrops,
                (unsigned long)s.eventDrops, (unsigned long)s.pollDrops,
                (unsigned long)s.joinFailures, (unsigned long)s.apStarts,
                (unsigned)s.lastReason,
                (unsigned long)s.lastFallbackMs,
                (unsigned long)s.worstFallbackMs,
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getMinFreeHeap());
}

static bool tryJoin(uint32_t timeoutMs) {
  Serial.printf("[hublink] joining \"%s\" ...\n", WIFI_STA_SSID);
  g_teardown = true;                 // mode change raises a spurious event
  WiFi.mode(WIFI_STA);
  // We own the fallback policy; the core's own reconnect would race it.
  WiFi.setAutoReconnect(false);
  WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);
  g_dropFlag = false;
  g_teardown = false;

  const uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) {
      g_hubIp    = WiFi.gatewayIP();
      g_linkUpMs = millis();
      g_dropFlag = false;            // drop the join-phase noise
      g_stats.staJoins++;
      Serial.printf("[hublink] STA up: ip=%s gw=%s rssi=%d\n",
                    WiFi.localIP().toString().c_str(),
                    g_hubIp.toString().c_str(), WiFi.RSSI());
      return true;
    }
    delay(50);
  }
  Serial.println("[hublink] hub network not found");
  g_stats.joinFailures++;
  g_teardown = true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  g_teardown = false;
  g_dropFlag = false;
  return false;
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
  webuiStop();
  g_state = HUBLINK_AP;
  webuiStart();
  g_stats.apStarts++;
  g_teardown = false;

  const uint32_t took = millis() - noticedMs;
  g_stats.lastFallbackMs = took;
  if (took > g_stats.worstFallbackMs) g_stats.worstFallbackMs = took;

  // A link that dropped may be a blip rather than a hub that went home. Retry
  // soon once, then settle back to the slow cadence so a genuinely absent hub
  // does not cost an AP teardown every ten seconds.
  g_retryMs  = WIFI_STA_QUICK_RETRY_MS;
  g_lastTry  = millis();
  g_linkUpMs = 0;
  hublinkPrintStats("fallback");
}

void hublinkBegin() {
  memset(&g_stats, 0, sizeof(g_stats));
  WiFi.onEvent(onWifiEvent);

  g_lastTry = millis();
  g_retryMs = WIFI_STA_RETRY_MS;
  if (tryJoin(WIFI_STA_TIMEOUT_MS)) {
    g_state = HUBLINK_STA;
    webuiStartOnCurrentNetwork();   // same routes, no AP
    hublinkPrintStats("boot-sta");
    return;
  }
  // Fall back to exactly what the board did before the hub existed. The logger
  // is a standalone product; the hub is an optional client.
  g_state = HUBLINK_AP;
  webuiStart();
  g_stats.apStarts++;
  hublinkPrintStats("boot-ap");
}

void hublinkLoop() {
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
  g_retryMs = WIFI_STA_RETRY_MS;     // the quick retry is a one-shot

  webuiStop();                       // free the radio; one mode at a time
  if (tryJoin(WIFI_STA_TIMEOUT_MS)) {
    g_state = HUBLINK_STA;
    webuiStartOnCurrentNetwork();
    hublinkPrintStats("rejoin");
  } else {
    webuiStart();                    // back to the AP
    g_stats.apStarts++;
  }
}

HubLinkState hublinkState() { return g_state; }

const char *hublinkStateName() {
  switch (g_state) {
    case HUBLINK_STA: return "sta";
    case HUBLINK_AP:  return "ap";
    default:          return "off";
  }
}

bool      hublinkOnHub() { return g_state == HUBLINK_STA; }
IPAddress hublinkHubIp() { return g_hubIp; }

const HubLinkStats *hublinkStats() { return &g_stats; }

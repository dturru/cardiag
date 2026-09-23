#include <Arduino.h>
#include <WiFi.h>

#include "hublink.h"
#include "webui.h"
#include "config.h"
#include "secrets.h"

static HubLinkState g_state    = HUBLINK_OFF;
static uint32_t     g_lastTry  = 0;
static IPAddress    g_hubIp;

// NEVER WIFI_AP_STA.
//
// The ESP32 has ONE radio. In AP+STA the two interfaces must share a channel,
// so joining a hub AP on channel 6 silently drags this board's own AP off
// WIFI_AP_CHANNEL. Clients that were told to expect channel 1 then fail in a
// way that looks like a range problem. Run one mode at a time.

static bool tryJoin(uint32_t timeoutMs) {
  Serial.printf("[hublink] joining \"%s\" ...\n", WIFI_STA_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);

  const uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) {
      g_hubIp = WiFi.gatewayIP();
      Serial.printf("[hublink] STA up: ip=%s gw=%s rssi=%d\n",
                    WiFi.localIP().toString().c_str(),
                    g_hubIp.toString().c_str(), WiFi.RSSI());
      return true;
    }
    delay(50);
  }
  Serial.println("[hublink] hub network not found");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return false;
}

void hublinkBegin() {
  g_lastTry = millis();
  if (tryJoin(WIFI_STA_TIMEOUT_MS)) {
    g_state = HUBLINK_STA;
    webuiStartOnCurrentNetwork();   // same routes, no AP
    return;
  }
  // Fall back to exactly what the board did before the hub existed. The logger
  // is a standalone product; the hub is an optional client.
  g_state = HUBLINK_AP;
  webuiStart();
}

void hublinkLoop() {
  if (g_state == HUBLINK_STA) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[hublink] STA lost; falling back to AP");
      webuiStop();
      g_state = HUBLINK_AP;
      webuiStart();
      g_lastTry = millis();
    }
    return;
  }

  if (g_state != HUBLINK_AP) return;

  // Periodically look for the hub again, so the logger joins when the car gets
  // home without needing a power cycle.
  if (millis() - g_lastTry < WIFI_STA_RETRY_MS) return;
  g_lastTry = millis();

  webuiStop();                       // free the radio; one mode at a time
  if (tryJoin(WIFI_STA_TIMEOUT_MS)) {
    g_state = HUBLINK_STA;
    webuiStartOnCurrentNetwork();
  } else {
    webuiStart();                    // back to the AP
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

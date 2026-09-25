#pragma once

// Network mode: join the hub as a station, or fall back to the board's own AP.
//
// NEVER WIFI_AP_STA -- one radio, shared channel. See hublink.cpp.

#include <stdint.h>
#include <IPAddress.h>

enum HubLinkState : uint8_t {
  HUBLINK_OFF = 0,
  HUBLINK_STA = 1,   // joined the hub network
  HUBLINK_AP  = 2,   // standalone, serving its own AP (pre-hub behaviour)
};

// Starts a join attempt and RETURNS -- it no longer blocks setup(). loop()
// drives it; on failure the board falls back to its own AP. Call once.
void hublinkBegin();

// Call from loop(). Drives the join/fallback state machine one step per call
// (at most one Wi-Fi mode switch, never a wait), handles STA loss and the
// periodic re-join attempts.
void hublinkLoop();

HubLinkState hublinkState();
const char  *hublinkStateName();
bool         hublinkOnHub();
IPAddress    hublinkHubIp();

// ---------------------------------------------------------------------------
// Transition counters.
//
// These exist for the AP<->STA soak test: 50 hotspot toggles produce a number,
// not an impression. They are also the only way to see a fault that RECOVERED,
// which is exactly the shape of the `netstack cb reg failed with 12308` event
// -- it did not stop anything, so nothing would have recorded it.
// ---------------------------------------------------------------------------

struct HubLinkStats {
  uint32_t staJoins;        // successful STA joins
  uint32_t staDrops;        // STA links lost (event or poll)
  uint32_t joinFailures;    // join attempts that timed out
  uint32_t apStarts;        // AP fallbacks started
  uint32_t eventDrops;      // drops caught by the disconnect EVENT
  uint32_t pollDrops;       // drops caught only by the WiFi.status() poll
  uint8_t  lastReason;      // last wifi_err_reason_t
  uint32_t lastDropMs;      // millis() when the last drop was noticed
  uint32_t lastFallbackMs;  // how long drop -> AP serving took, ms
  uint32_t worstFallbackMs; // worst of the above this boot
  uint32_t lastLinkUpMs;    // how long the last STA link stayed up, ms
};

const HubLinkStats *hublinkStats();

// One line of soak-readable telemetry. Printed on every transition, so the
// serial log alone answers "how many, how fast, and did the heap move".
void hublinkPrintStats(const char *what);

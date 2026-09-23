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

// Tries STA first, falls back to webuiStart(). Call once from setup().
void hublinkBegin();

// Call from loop(). Handles STA loss and periodic re-join attempts.
void hublinkLoop();

HubLinkState hublinkState();
const char  *hublinkStateName();
bool         hublinkOnHub();
IPAddress    hublinkHubIp();

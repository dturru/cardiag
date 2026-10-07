#pragma once

// Hub discovery runtime: an asynchronous mDNS A-record query for
// <hub_name>.local, the selection order in hubaddr.h, and the hub-silence
// trigger. The same mDNS instance also ADVERTISES this logger as
// <logger_name>.local with a _cardiag._tcp service (TXT device_id), so the hub
// can find the logger by name too. Runs on the loop task and never waits: the query is started, then
// polled with a zero timeout on later passes. The CAN task is not involved.

#include <stddef.h>
#include <stdint.h>

#include "hubaddr.h"

// STA link up: (re)select now from what is known, then resolve.
// gateway/nvs are host-order IPv4 (hubaddr.h), 0 = absent.
void hubResolveLinkUp(uint32_t gateway, uint32_t nvs);
// STA link lost: cancel any query in flight (parked until it finishes, then
// freed -- hubquery.h). The cache is kept.
void hubResolveLinkDown();
// loop(), while on the hub network. Non-blocking.
void hubResolveLoop();
// loop(), EVERY pass, on or off the hub network: reap a parked query.
void hubResolveReap();

// The last few mDNS actions with their age, e.g. "query+3210ms answer+1180ms"
// (newest last), for the heap integrity check's failure line (heapdiag.h).
void hubResolveDescribeRecent(char *out, size_t cap, uint32_t now);

// A /api/v1 request arrived from `remote` (host order). From the current hub
// address it proves the hub is reachable; from any other address it is a hint
// the hub moved, and triggers a re-resolve (rate-limited).
void hubResolveNoteRequest(uint32_t remote);

uint32_t      hubResolveIp();       // host order; 0 = none
HubAddrSource hubResolveSource();
const char   *hubResolveName();     // "<hub_name>.local"
const char   *hubResolveSelfName(); // "<logger_name>.local"

struct HubMdnsStats {
  uint32_t queries, answers, failures;
  uint32_t parked, reaped;           // cancelled in flight at link-down / freed later
  uint32_t lastAnswerIp;             // host order, the cache; 0 = never
  uint32_t lastQueryMs;              // how long the last query took
  bool     initOk;
  bool     advertised;               // hostname + _cardiag._tcp registered
  // Measured around mdns_init() + the advertisement: what the component
  // costs this board, responder included.
  uint32_t heapBefore, heapAfter, largestBefore, largestAfter;
};
const HubMdnsStats *hubResolveStats();

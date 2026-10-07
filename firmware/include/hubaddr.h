#pragma once

// ---------------------------------------------------------------------------
// WHERE IS THE HUB? Selection order and re-resolve scheduling, pure.
//
// On a laptop hotspot the hub's IP changes between sessions (no DHCP
// reservations), so a fixed `hub_addr` goes stale. The hub announces itself
// over mDNS as <hub_name>.local (default carhub.local), so the logger asks:
//
//   1. mDNS        a fresh answer to <hub_name>.local
//   2. mdns-cache  the last good mDNS answer this boot, while a re-query fails
//   3. nvs         `cred set hub_addr` -- manual override / last known
//   4. gateway     the Wi-Fi gateway, which IS the hub on the hub's own AP
//
// When to ask: when the session (STA link) starts, and whenever the hub looks
// unreachable -- no /api/v1 request from the current hub address for
// HUB_SILENT_MS (the hub polls /api/v1/session every 30 s), or a request from
// a different address (a strong hint it moved). Failed queries back off.
//
// Pure, no Arduino: test/test_hubaddr drives it natively. hublink.cpp runs
// the (asynchronous, non-blocking) mDNS query.
// ---------------------------------------------------------------------------

#include <stdint.h>

enum HubAddrSource : uint8_t {
  HUBADDR_NONE = 0,     // no candidate at all (not on a network)
  HUBADDR_MDNS,
  HUBADDR_MDNS_CACHE,
  HUBADDR_NVS,
  HUBADDR_GATEWAY,
};

static inline const char *hubAddrSourceName(HubAddrSource s) {
  switch (s) {
    case HUBADDR_MDNS:       return "mdns";
    case HUBADDR_MDNS_CACHE: return "mdns-cache";
    case HUBADDR_NVS:        return "nvs";
    case HUBADDR_GATEWAY:    return "gateway";
    default:                 return "none";
  }
}

// IPv4 as a host-order u32 (a.b.c.d = a<<24 | ...); 0 = absent.
struct HubAddrInputs {
  uint32_t mdns;        // answer from the most recent query; 0 if it failed
  uint32_t mdnsCache;   // last good answer this boot; 0 if never
  uint32_t nvs;         // cred hub_addr; 0 if unset
  uint32_t gateway;     // WiFi.gatewayIP(); 0 if not on a network
};

struct HubAddrChoice {
  uint32_t      ip;
  HubAddrSource source;
};

static inline bool hubAddrUsable(uint32_t ip) {
  return ip != 0 && ip != 0xFFFFFFFFu;
}

static inline HubAddrChoice hubAddrSelect(const HubAddrInputs &in) {
  if (hubAddrUsable(in.mdns))      return {in.mdns, HUBADDR_MDNS};
  if (hubAddrUsable(in.mdnsCache)) return {in.mdnsCache, HUBADDR_MDNS_CACHE};
  if (hubAddrUsable(in.nvs))       return {in.nvs, HUBADDR_NVS};
  if (hubAddrUsable(in.gateway))   return {in.gateway, HUBADDR_GATEWAY};
  return {0, HUBADDR_NONE};
}

// ---------------------------------------------------------------------------
// Re-resolve scheduling. One query at a time; a success parks the scheduler
// until the next kick; a failure retries at baseMs << fails, capped at maxMs.
// A kick never jumps a backoff that is already running, and is ignored within
// minGapMs of the last attempt -- a silent hub must not turn into a query
// storm.
// ---------------------------------------------------------------------------

struct HubResolveSched {
  bool     armed;       // a query is wanted at nextMs
  bool     inflight;    // a query is running
  uint32_t nextMs;
  uint32_t lastStartMs;
  bool     everStarted;
  uint8_t  fails;       // consecutive failed queries
};

static inline void hubResolveReset(HubResolveSched &s) {
  s = HubResolveSched{};
}

// Session start or "hub unreachable": ask soon.
static inline void hubResolveKick(HubResolveSched &s, uint32_t now,
                                  uint32_t minGapMs) {
  if (s.inflight) return;
  if (s.armed && s.fails > 0) return;            // backoff already running
  uint32_t at = now;
  if (s.everStarted && (int32_t)(now - s.lastStartMs) < (int32_t)minGapMs)
    at = s.lastStartMs + minGapMs;
  if (!s.armed || (int32_t)(at - s.nextMs) < 0) s.nextMs = at;
  s.armed = true;
}

static inline bool hubResolveDue(const HubResolveSched &s, uint32_t now) {
  return s.armed && !s.inflight && (int32_t)(now - s.nextMs) >= 0;
}

static inline void hubResolveStarted(HubResolveSched &s, uint32_t now) {
  s.inflight = true;
  s.armed = false;
  s.lastStartMs = now;
  s.everStarted = true;
}

static inline uint32_t hubResolveBackoffMs(uint8_t fails, uint32_t baseMs,
                                           uint32_t maxMs) {
  uint64_t b = baseMs;
  for (uint8_t k = 1; k < fails && b < maxMs; k++) b <<= 1;
  return b > maxMs ? maxMs : (uint32_t)b;
}

static inline void hubResolveFinished(HubResolveSched &s, uint32_t now, bool ok,
                                      uint32_t baseMs, uint32_t maxMs) {
  s.inflight = false;
  if (ok) {
    s.fails = 0;
    s.armed = false;                             // parked until the next kick
    return;
  }
  if (s.fails < 16) s.fails++;
  s.armed = true;
  s.nextMs = now + hubResolveBackoffMs(s.fails, baseMs, maxMs);
}

// The hub polls /api/v1/session; no request from it for silentMs (counted
// from the later of link-up and its last request) = unreachable.
static inline bool hubSilent(uint32_t now, uint32_t lastSeenMs,
                             uint32_t linkUpMs, uint32_t silentMs) {
  const uint32_t since =
      (int32_t)(lastSeenMs - linkUpMs) > 0 ? lastSeenMs : linkUpMs;
  return (int32_t)(now - since) >= (int32_t)silentMs;
}

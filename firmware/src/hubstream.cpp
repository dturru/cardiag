#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include "hubstream.h"
#include "hublink.h"
#include "hubproto.h"
#include "hubchunk.h"
#include "session.h"
#include "sniffer.h"
#include "config.h"

static WiFiUDP  g_udp;
static uint32_t g_seq         = 0;
static uint32_t g_nextSnapMs  = 0;
static uint32_t g_nextFastMs  = 0;
static uint32_t g_lastSnapMs  = 0;   // watermark for "changed since"
static uint32_t g_lastFastMs  = 0;
static bool     g_wantFull    = true;
static uint32_t g_packets     = 0;
static uint32_t g_records     = 0;
static uint32_t g_fastPackets = 0;
static uint32_t g_fastRecords = 0;
static bool     g_began       = false;

// The fast list: ids streamed at up to HUB_FAST_HZ instead of HUB_SNAPSHOT_HZ,
// for anything that drives a live indicator (the shift light wants RPM). Empty
// by default -- the Civic's id semantics are UNKNOWN, so nothing is assumed
// here. The hub configures it once an id is established.
static uint32_t g_fastIds[HUB_FAST_MAX_IDS];
static uint16_t g_fastCount = 0;

static uint8_t  g_buf[HUB_MAX_PACKET];

void hubstreamBegin() {
  g_seq = 0;
  g_wantFull = true;
  g_began = false;
  g_fastCount = 0;
  g_packets = g_records = g_fastPackets = g_fastRecords = 0;
}

void hubstreamRequestFullSnapshot() { g_wantFull = true; }

uint32_t hubstreamPacketsSent()     { return g_packets; }
uint32_t hubstreamRecordsSent()     { return g_records; }
uint32_t hubstreamFastPacketsSent() { return g_fastPackets; }
uint32_t hubstreamFastRecordsSent() { return g_fastRecords; }

uint16_t hubstreamSetFastIds(const uint32_t *ids, uint16_t n) {
  if (!ids || n == 0) { g_fastCount = 0; return 0; }
  if (n > HUB_FAST_MAX_IDS) n = HUB_FAST_MAX_IDS;
  for (uint16_t i = 0; i < n; i++) g_fastIds[i] = ids[i];
  g_fastCount = n;
  // Send the new ids immediately rather than waiting for one of them to
  // change: a hub that just asked for an id wants its current value, and an id
  // that is sitting still would otherwise look like the list was ignored.
  g_lastFastMs = 0;
  g_nextFastMs = millis();
  return n;
}

uint16_t hubstreamFastIds(uint32_t *out, uint16_t cap) {
  const uint16_t n = (cap < g_fastCount) ? cap : g_fastCount;
  for (uint16_t i = 0; i < n; i++) out[i] = g_fastIds[i];
  return n;
}

uint16_t hubstreamFastCount() { return g_fastCount; }

// The transport half. Serialisation lives in hubchunk.cpp, which is host-
// testable precisely because it knows nothing about UDP.
static bool emitUdp(const uint8_t *packet, size_t len, void *ctx) {
  const bool fast = *(const bool *)ctx;
  if (!g_udp.beginPacket(hublinkHubIp(), HUB_UDP_PORT)) return false;
  g_udp.write(packet, len);
  g_udp.endPacket();

  const uint16_t recs =
      (uint16_t)((len - HUB_HEADER_LEN) / HUB_RECORD_LEN);
  g_packets++;
  g_records += recs;
  if (fast) { g_fastPackets++; g_fastRecords += recs; }
  return true;
}

static void sendRows(const SnifferRow *rows, uint16_t n, bool fast, bool full) {
  uint8_t flags = 0;
  if (full) flags |= HUB_FLAG_FULL_SNAPSHOT;
  if (fast) flags |= HUB_FLAG_FAST;

  bool fastCtx = fast;
  hubChunkRows(g_buf, sizeof(g_buf), rows, n,
               sessionDeviceId(), sessionBootId(), &g_seq, millis(),
               flags, emitUdp, &fastCtx);
}

void hubstreamLoop() {
  if (!hublinkOnHub()) {
    // Not on the hub network. Reset so the next connect re-announces.
    g_began = false;
    g_wantFull = true;
    return;
  }

  if (!g_began) {
    g_udp.begin(0);              // ephemeral source port
    g_began = true;
    g_nextSnapMs = g_nextFastMs = millis();
  }

  const uint32_t now = millis();
  static SnifferRow rows[SNIFF_MAX_IDS];

  // Protocol 1.5: a connecting hub gets one FULL_SNAPSHOT of every known id.
  if (g_wantFull) {
    const uint16_t n = snifferRowsChangedSince(rows, SNIFF_MAX_IDS, 0);
    if (n) {
      sendRows(rows, n, /*fast=*/false, /*full=*/true);
      g_wantFull   = false;
      g_lastSnapMs = now;
      g_lastFastMs = now;
      g_nextSnapMs = now + (1000u / HUB_SNAPSHOT_HZ);
      g_nextFastMs = now + (1000u / HUB_FAST_HZ);
    }
    return;
  }

  if (g_fastCount && (int32_t)(now - g_nextFastMs) >= 0) {
    g_nextFastMs = now + (1000u / HUB_FAST_HZ);
    const uint16_t n = snifferRowsForIds(rows, SNIFF_MAX_IDS,
                                         g_fastIds, g_fastCount, g_lastFastMs);
    if (n) { sendRows(rows, n, /*fast=*/true, /*full=*/false); g_lastFastMs = now; }
  }

  if ((int32_t)(now - g_nextSnapMs) >= 0) {
    g_nextSnapMs = now + (1000u / HUB_SNAPSHOT_HZ);
    const uint16_t n = snifferRowsChangedSince(rows, SNIFF_MAX_IDS, g_lastSnapMs);
    if (n) { sendRows(rows, n, /*fast=*/false, /*full=*/false); g_lastSnapMs = now; }
  }
}

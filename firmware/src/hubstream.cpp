#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include "hubstream.h"
#include "hublink.h"
#include "hubproto.h"
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
}

void hubstreamRequestFullSnapshot() { g_wantFull = true; }

uint32_t hubstreamPacketsSent() { return g_packets; }
uint32_t hubstreamRecordsSent() { return g_records; }

// Serialises up to HUB_MAX_RECORDS rows into one datagram and sends it.
// Chunked: a full snapshot of 64 ids still fits one packet, but the loop is
// written so a larger table would split rather than truncate.
static void sendRows(const SnifferRow *rows, uint16_t n, bool fast, bool full) {
  uint16_t sent = 0;
  do {
    const uint16_t chunk = (uint16_t)((n - sent) > HUB_MAX_RECORDS
                                          ? HUB_MAX_RECORDS : (n - sent));
    uint8_t flags = 0;
    if (full) flags |= HUB_FLAG_FULL_SNAPSHOT;
    if (fast) flags |= HUB_FLAG_FAST;

    HubHeader *h = (HubHeader *)g_buf;
    hubHeaderInit(h, sessionDeviceId(), sessionBootId(), g_seq++,
                  millis(), flags, chunk);

    HubCanRecord *rec = (HubCanRecord *)(g_buf + HUB_HEADER_LEN);
    for (uint16_t i = 0; i < chunk; i++) {
      const SnifferRow &r = rows[sent + i];
      hubCanRecord(&rec[i], r.id, r.ext, r.dlc, r.data, r.changedMask, r.lastMs);
    }

    const size_t len = HUB_HEADER_LEN + (size_t)chunk * HUB_RECORD_LEN;
    if (g_udp.beginPacket(hublinkHubIp(), HUB_UDP_PORT)) {
      g_udp.write(g_buf, len);
      g_udp.endPacket();
      g_packets++;
      g_records += chunk;
    }
    sent += chunk;
  } while (sent < n);
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

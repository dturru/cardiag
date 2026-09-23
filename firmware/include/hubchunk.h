#pragma once

// Packet serialisation for the hub UDP stream. Protocol v1 §1.2-1.3.
//
// WHY THIS IS ITS OWN MODULE
//
// This logic used to live inside hubstream.cpp's sendRows(), interleaved with
// WiFiUDP calls. That made the split path -- more rows than fit one datagram
// -- impossible to test anywhere except on a board with a hub listening, which
// in practice meant it was never tested at all. Worse, it is currently
// UNREACHABLE in the firmware by construction: SNIFF_MAX_IDS (64) equals
// HUB_MAX_RECORDS (64), so a snapshot can never produce a 65th row.
//
// Unreachable is not the same as correct. The table WILL grow -- a BMW has
// more ids than a Civic -- and the first time it does, this loop runs for the
// first time on a live vehicle bus. So it is separated from the transport and
// tested on the host with synthetic rows, where 65, 128 and 129 records are
// one line each.
//
// Nothing here touches Arduino, WiFi or FreeRTOS: only stdint, string.h and
// hubproto.h. That is a constraint, not an accident -- it is what makes
// `pio test -e native` possible.

#include <stdint.h>
#include <stddef.h>

#include "hubproto.h"
#include "snifferrow.h"

// Called once per datagram, with a fully serialised packet. Returning false
// means "the transport refused it"; chunking stops there rather than carrying
// on burning sequence numbers on packets nobody will ever see.
typedef bool (*HubEmitFn)(const uint8_t *packet, size_t len, void *ctx);

struct HubChunkResult {
  uint16_t packets;    // datagrams handed to emit()
  uint16_t records;    // records serialised across all of them
  bool     aborted;    // emit() returned false partway
};

// Serialises `n` rows into as many datagrams as it takes, at most
// HUB_MAX_RECORDS per packet, and hands each to `emit`.
//
// `seq` is advanced once per PACKET, not once per call: the protocol's
// sequence number detects datagram loss, so a split that reused one number
// would make a lost second half invisible.
//
// `buf` must be at least HUB_MAX_PACKET bytes. Passing fewer is a caller bug
// and returns zero packets rather than writing past the end.
HubChunkResult hubChunkRows(uint8_t *buf, size_t bufCap,
                            const SnifferRow *rows, uint16_t n,
                            uint32_t deviceId, uint32_t bootId,
                            uint32_t *seq, uint32_t uptimeMs,
                            uint8_t flags,
                            HubEmitFn emit, void *ctx);

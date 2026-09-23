#pragma once

// UDP live stream to the hub. Protocol v1 section 1.
//
// A LATEST-VALUE SNAPSHOT, not a frame stream: the bus runs ~1000 fps and no
// 5 Hz link carries that. The sniffer's per-ID table already holds exactly what
// a snapshot is, so this module paces it and serialises it.

#include <stdint.h>

void hubstreamBegin();

// Call from loop(). Cheap no-op when not on the hub network.
void hubstreamLoop();

// Forces the next packet to be a FULL_SNAPSHOT (protocol 1.5: a hub that has
// just connected gets every known id, not just the changed ones).
void hubstreamRequestFullSnapshot();

uint32_t hubstreamPacketsSent();
uint32_t hubstreamRecordsSent();

// Packets carrying HUB_FLAG_FAST, and the records in them. Kept apart from the
// totals so "is the fast list actually running at HUB_FAST_HZ" is answerable
// from the logger's own numbers rather than only by counting datagrams at the
// hub -- if the two disagree, the difference is the network, which is a
// different bug from the list not being applied.
uint32_t hubstreamFastPacketsSent();
uint32_t hubstreamFastRecordsSent();

// ---------------------------------------------------------------------------
// Fast list (protocol 1.5). Ids streamed at up to HUB_FAST_HZ instead of
// HUB_SNAPSHOT_HZ, for anything driving a live indicator.
//
// Empty by default and it must stay that way on a real bus: the logger does
// not know what any id means (protocol rule 3), so it cannot choose. The hub
// sets the list once an id is established. MODE_SELFTEST is the one exception,
// and it is a bench fixture -- see selftest_profile.h.
// ---------------------------------------------------------------------------

// Replaces the list. `n` above HUB_FAST_MAX_IDS is truncated; n == 0 clears.
// Returns how many ids were accepted.
uint16_t hubstreamSetFastIds(const uint32_t *ids, uint16_t n);

uint16_t hubstreamFastIds(uint32_t *out, uint16_t cap);
uint16_t hubstreamFastCount();

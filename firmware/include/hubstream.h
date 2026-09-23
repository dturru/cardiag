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

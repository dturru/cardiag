#pragma once

// Two records, both living in PSRAM.
//
// RAW RING -- every frame, always rolling, oldest overwritten. This is the
// pre-trigger buffer: you cannot decide to record the interesting thirty
// seconds after they have happened, so keep them always and throw them away
// when nothing came of it. At ~1000 fps and 16 bytes a frame, 4 MB is roughly
// four minutes of history at full fidelity.
//
// CHANGE LOG -- tier 1, the continuous record. A frame is appended only when a
// non-heartbeat byte differs from the previous frame of the same ID. The bus is
// overwhelmingly repetition: most IDs resend an identical payload every 10 ms,
// so this is lossless for state changes while writing a small fraction of the
// frames. It is deduplication, not sampling. Fixed-rate sampling would alias
// away exactly the transients this project exists to catch.
//
// Both are VOLATILE -- power loss loses them. Persisting to flash or SD is a
// later tier; for now the workflow is drive, then download over the AP.

#include <stdint.h>
#include <stddef.h>
#include "driver/twai.h"

// Allocates both buffers. Falls back to much smaller heap buffers when PSRAM is
// unavailable, so the feature degrades instead of vanishing.
void recorderBegin();

bool     recorderHasPsram();
size_t   recorderRawCapacity();      // frames
size_t   recorderChangeCapacity();   // entries

// Every received frame. Always rolling; there is no start/stop for the ring.
void recorderNoteRaw(const twai_message_t &msg);

// Called by the sniffer when a non-heartbeat byte moved. `changed` is the mask
// of bytes that differ from the previous frame of this ID.
void recorderNoteChange(uint32_t id, bool extd, uint8_t dlc,
                        const uint8_t *data, uint8_t changed);

void recorderStart();     // begin appending to the change log
void recorderStop();
bool recorderRunning();
void recorderClear();     // empties the change log; leaves the ring alone

uint32_t recorderRawStored();      // frames currently held (<= capacity)
uint32_t recorderRawTotal();       // frames ever seen, including overwritten
uint32_t recorderChangeStored();
uint32_t recorderChangeDropped();  // appends refused because the log filled

// While frozen the raw ring stops accepting frames. A download takes seconds
// and the ring wraps every few minutes, so without this a long transfer could
// have the ground move under it and emit rows twice or not at all. Frames are
// lost during the transfer, which is the right trade for a coherent file.
void recorderFreezeRaw(bool freeze);
bool recorderRawFrozen();

// CSV, emitted in chunks so a multi-megabyte download never needs to exist in
// RAM at once. Zero-initialise the cursor and call until the result is 0.
struct RecCsvCursor {
  uint32_t row;
  bool     headerDone;
};

size_t recorderRawCsvChunk(char *out, size_t cap, RecCsvCursor *cur);
size_t recorderChangeCsvChunk(char *out, size_t cap, RecCsvCursor *cur);

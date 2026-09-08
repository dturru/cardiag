#pragma once

// Per-ID sniffer table.
//
// Printing every frame is unusable on a real bus: a 2012 Civic idles around
// 1000 frames/s across ~40 IDs, which is both unreadable and lossy, because
// USB CDC with setTxTimeoutMs(0) drops rather than blocks. So aggregate at the
// source. One row per ID, updated in place, printed a couple of times a second.
//
// The point of the table is not the frame list. It is answering "which byte
// moved when I pressed the brake" -- so every row carries a baseline captured
// at the last clear, and any byte differing from that baseline stays marked
// until the next clear. Press clear, do the thing, then read the screen. That
// works alone in a car; watching for a transient highlight does not.

#include <stdint.h>
#include "driver/twai.h"

// Creates the lock guarding the table. Call once from setup(), before the
// receive task starts. The table is written by the CAN task and read by the
// web handler, so every entry point below takes that lock internally.
void snifferBegin();

// Records one frame. Cheap enough to call on every RX.
void snifferNote(const twai_message_t &msg);

// Re-baselines every row to its current payload and clears the change marks.
void snifferClearMarks();

// Forgets every ID. For moving to a different vehicle or bus.
void snifferReset();

// Prints one table block. Bus counters are passed in rather than read here so
// the sniffer stays a pure table and the driver stats keep one owner.
void snifferPrint(uint32_t frames, uint32_t missed, uint32_t busErr);

uint16_t snifferIdCount();
uint16_t snifferOverflow();

// Serialises the whole table as JSON into `out`. Returns bytes written.
// Byte state travels as two bitmasks per row rather than pre-rendered text, so
// the page decides how to draw it and the payload stays small.
size_t snifferSnapshotJson(char *out, size_t cap,
                           uint32_t frames, uint32_t missed, uint32_t busErr);

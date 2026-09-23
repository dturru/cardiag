#pragma once

// ---------------------------------------------------------------------------
// Phase B filestore -- protocol v1 §2, on LittleFS.
//
// This is the TRUTH path. Rule 2 of the system spec: files are truth, UDP is
// display-only, and no hub failure may cost baseline data. Until now the
// logger held everything in volatile PSRAM and a power cut lost the drive, so
// "the logger is standalone" was true about the UI and not about the data.
//
// ---------------------------------------------------------------------------
// TIER vs KIND -- these are two different things and conflating them is what
// produced the naming drift this header now fixes.
//
//   TIER  = the RETENTION CLASS. It is the only thing the protocol's deletion
//           order cares about, and it is the letter in the filename.
//   KIND  = WHAT IS IN THE FILE, which decides how it parses.
//
// A tier can hold more than one kind. Tier A holds two, and that is the whole
// reason the two concepts had to be separated: the raw ring and the change log
// are both frame-level records of the bus, so they are both Tier A for
// retention purposes, but they are different files with different formats.
//
//   Tier  Kind        Rate            Format  Phase B
//   ----  ----------  --------------  ------  ------------------------------
//   A     raw         ~16 kB/s        CSV     NO -- RAM only, 4 MB of ring
//                                             does not fit 3.5 MB of flash
//   A     changes     9,450 B/s meas  CSV     YES -- ~7 min of partition
//   B     snapshot    4 + 13N B/s     CDGS    YES -- 186 B/s @ 14 ids, HOURS
//   C     bookend     bytes per trip  CSV     NO -- trip DTCs / Mode 06, not
//                                             implemented yet
//
// ⚠ EARLIER DRAFTS OF THIS MODULE CALLED THE CHANGE LOG "TIER C". That was
// wrong: Tier C is the trip bookends. The change log is deduplicated raw
// frames and has always belonged to Tier A. docs/hardware.md never assigned it
// a letter at all, which is why the drift went unnoticed.
//
// DELETION ORDER, and the reasoning is by freeable bytes x replaceability, not
// by sentiment:
//   1. acked, oldest first      -- the hub has it; free
//   2. unacked Tier A           -- large, and collectable again on the next
//                                  drive
//   3. unacked Tier B           -- large and irreplaceable; a month that is
//                                  gone cannot be re-measured
//   4. unacked Tier C           -- LAST, and mostly a formality: bookends are
//                                  bytes per trip, so deleting them frees
//                                  nothing. If C is what stands between you
//                                  and a full disk, the disk is not the problem
//
// Tier A is additionally CAPPED (FS_TIER_A_MAX_PCT) so it can never crowd
// Tier B out in the first place. Ordering alone is not enough: Tier A outruns
// Tier B roughly fifty to one and would fill the partition between two
// snapshot blocks.
// ---------------------------------------------------------------------------
//
// 🔑 LittleFS is a STAGING BUFFER, not the destination. The carrier board's
// microSD is still on the critical path -- 3.5 MB of internal flash is minutes
// of Tier C, and the only reason the numbers work at all is Tier B's 1 Hz
// sampling. This module is written so the storage backend can move to SD
// without the hub noticing.
// ---------------------------------------------------------------------------

#include <stdint.h>
#include <stddef.h>

class WebServer;

// Tier letters -- the RETENTION class. These appear in the filename and in the
// listing's "tier" field, and they are what the deletion order sorts on.
#define FS_TIER_RAW      'A'     // frame-level record of the bus
#define FS_TIER_SNAPSHOT 'B'     // 1 Hz snapshot / health
#define FS_TIER_BOOKEND  'C'     // trip bookends (DTCs, Mode 06) -- not yet

// Kind letters -- WHAT IS IN THE FILE. Second character of the filename and
// the listing's "kind" field. Tier A has two kinds; the rest have one each.
#define FS_KIND_RAW      'r'     // every frame (Tier A) -- not written yet
#define FS_KIND_CHANGES  'c'     // change log (Tier A)
#define FS_KIND_SNAPSHOT 's'     // snapshot log (Tier B)
#define FS_KIND_BOOKEND  'b'     // trip bookend (Tier C) -- not written yet

// The producing mode is not recoverable for this file.
//
// It is a distinct value and NOT a synonym for "a real capture". A closed file
// carries `mode=` in its .meta and a Tier B .part carries it in the CDGS
// header, but a CSV .part orphaned by a reset has nowhere to have kept it. The
// listing reports that as `"synthetic":null`, because claiming `false` would
// assert the file came from the car.
#define FS_MODE_UNKNOWN  0xFF

// The one place the mapping lives. Used by the writers and by the scanner, so
// a file's tier can never disagree with its kind.
static inline char fsTierForKind(char kind) {
  switch (kind) {
    case FS_KIND_SNAPSHOT: return FS_TIER_SNAPSHOT;
    case FS_KIND_BOOKEND:  return FS_TIER_BOOKEND;
    default:               return FS_TIER_RAW;   // raw and changes
  }
}

static inline const char *fsKindName(char kind) {
  switch (kind) {
    case FS_KIND_RAW:      return "raw";
    case FS_KIND_CHANGES:  return "changes";
    case FS_KIND_SNAPSHOT: return "snapshot";
    case FS_KIND_BOOKEND:  return "bookend";
    default:               return "unknown";
  }
}

struct FileStoreStats {
  uint32_t usedBytes;
  uint32_t totalBytes;
  uint16_t files;
  uint16_t openFiles;
  uint32_t nextIndex;
  int32_t  ackedThrough;     // -1 = nothing acked yet
  uint32_t tierABytes;
  uint32_t tierBBytes;
  uint32_t tierCBytes;
  uint32_t deletedAcked;
  uint32_t deletedUnacked;   // >0 means data was lost to retention
  uint32_t writeErrors;
  uint32_t rowsDropped;      // change-log appends the recorder refused
  bool     mounted;
};

// Mounts LittleFS, rebuilds the index by scanning FS_DIR, and loads the
// watermark from NVS. Safe to call when the partition is empty or corrupt --
// it reports and degrades rather than refusing to boot.
bool filestoreBegin();

// Call from loop(). Writes the 1 Hz snapshot block, drains the recorder's
// change log, rotates files and enforces retention. Bounded per call so a
// flush never stalls the web server or the UDP stream.
void filestoreLoop();

// Whether new data is being written at all. Closing the active files on the
// way out means a mode change never leaves a half-written file behind.
void filestoreSetEnabled(bool on);
bool filestoreEnabled();

// The mode character stamped into new files, so bench data is identifiable as
// bench data forever. SELFTEST output is marked synthetic in the listing.
void filestoreSetMode(uint8_t mode);

// Closes whatever is open. Called before a mode change and from the API.
void filestoreCloseActive();

const FileStoreStats *filestoreStats();

// Percent of the partition in use, 0-100.
uint8_t filestoreUsagePct();

// Registers /api/v1/files, /api/v1/files/<index> and /api/v1/files/ack.
void filestoreRegister(WebServer &srv);

// Applies a watermark ack (protocol §2.1). Idempotent, never moves backwards.
// Returns the watermark actually in force afterwards.
int32_t filestoreAck(int32_t throughIndex);

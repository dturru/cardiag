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
  // Closed files the hub has not acked. Between trips this is normally
  // NON-ZERO and that is correct, not a stalled sync: the bus goes quiet, the
  // files close ~3 s later and the board sleeps before the hub can pull them,
  // so a trip syncs at the NEXT ignition (protocol 2.3.1).
  //
  // The logger has to report it because the hub cannot derive it -- evictions
  // punch holes in the index range, so files/open/acked_through do not
  // determine the count.
  uint16_t pendingUnacked;
  uint32_t nextIndex;
  int32_t  ackedThrough;     // -1 = nothing acked yet
  uint32_t tierABytes;
  uint32_t tierBBytes;
  uint32_t tierCBytes;
  // SINCE BOOT. Unchanged meaning, kept for protocol compatibility.
  uint32_t deletedAcked;
  uint32_t deletedUnacked;

  // ⭐ LIFETIME, MONOTONIC, NVS-BACKED. Per tier, indexed by fsTierSlot().
  //
  // Two separate reasons these exist and are not just `deletedUnacked`:
  //
  // 1. PER TIER, because the tiers are not interchangeable. Tier A is
  //    collectable again on the next drive; Tier B is a month that cannot be
  //    re-measured. One total cannot tell them apart.
  //
  // 2. LIFETIME AND PERSISTENT, because the since-boot counters are erased by
  //    exactly the event that ends every trip. The transceiver's INH pin
  //    removes power at key-off, so a loss during a drive -- whose files the
  //    hub will not collect until the NEXT ignition (protocol 2.3.1) -- was
  //    reported to nobody and then forgotten. The logger came back up saying
  //    deleted_unacked = 0 while the data was still missing. Measured
  //    2026-09-24: a mid-run reboot reset a count of 22 to 0.
  //
  // These only ever count up. They are never reset, including by a format --
  // a format is the most destructive thing this device does, so it is the
  // last moment to forget that data was lost.
  uint64_t lostBytes[3];
  uint32_t lostFiles[3];

  // The hub's watermark over lostFiles, also lifetime and NVS-backed: "I have
  // recorded this many lost files." Not an acknowledgement that the data came
  // back -- it cannot -- but that the loss is now written down somewhere that
  // survives. `warn` stays true until this catches up, so a key-off can no
  // longer erase the warning.
  uint32_t lostAckedFiles;

  uint32_t writeErrors;
  uint32_t rowsDropped;      // change-log appends the recorder refused
  bool     mounted;

  // ⭐ THE BOOT SCAN, AS MEASURED ON THIS BOOT. FS_BOOT_WDT_S (config.h) is
  // derived from these, not guessed -- a scan that gets close to its budget
  // is visible on /api/v1/session before it is a watchdog reset.
  uint32_t mountMs;
  uint32_t scanMs;
  uint32_t scanEntries;      // everything readdir() returned
  uint32_t scanFiles;        // .log + .part among them
  uint32_t scanForeign;      // names that are not ours (reported, never deleted)
  uint32_t scanEvictedForRoom;  // table could not grow; evicted, never dropped
  // Entries whose size/digest/mode have not been read yet. The scan reads
  // names only; filestoreLoop() fills the rest in a few per pass. Tier byte
  // totals are partial until this reaches zero.
  uint16_t unhydrated;

  // SAFE MODE (bootguard.h). The filestore was not started on this boot
  // because the last BG_FAIL_LIMIT starts never finished. Everything above
  // except the NVS-backed loss record is then zero because nothing was read.
  bool     safeMode;
  uint8_t  bootAttempts;     // unfinished starts before this boot
  uint32_t erases;           // lifetime, NVS: remote erases performed
};

// Total lost files across all tiers, lifetime. The quantity the hub's
// watermark is compared against.
static inline uint32_t fsLostFilesTotal(const struct FileStoreStats *s) {
  return s->lostFiles[0] + s->lostFiles[1] + s->lostFiles[2];
}

// Index into the per-tier arrays above. Anything unrecognised lands in A,
// matching fsTierForKind()'s default.
static inline uint8_t fsTierSlot(char tier) {
  switch (tier) {
    case FS_TIER_SNAPSHOT: return 1;
    case FS_TIER_BOOKEND:  return 2;
    default:               return 0;
  }
}

// Loads the watermark and the lifetime loss record from NVS, mounts LittleFS
// and rebuilds the index from the directory NAMES (no file is opened). Returns
// false on a mount failure, which it reports and degrades on rather than
// refusing to boot.
//
// ⚠️ NEVER FORMATS. A partition that will not mount stays unmounted; the boot
// guard counts it as a failed start and, after BG_FAIL_LIMIT of them, the
// board comes up in safe mode where a person or the hub can decide to erase.
//
// Bounded by the early task watchdog (FS_BOOT_WDT_S), armed by the caller, so
// it deliberately does NOT feed the watchdog: a start that is still running
// when the budget runs out is exactly what the boot guard exists to catch.
bool filestoreBegin();

// Safe mode: the filestore is NOT started. Loads only the NVS loss record, so
// /api/v1/session still reports what has been lost rather than a confident
// zero, and marks the store failed.
void filestoreBeginSafeMode(uint8_t priorAttempts);

bool filestoreSafeMode();

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

// Called from the CAN task on every received frame. Cheap on purpose: one
// volatile store, because it is on the hot path at ~1,500 frames/s.
void filestoreNoteBusActivity();

// True once the bus has been quiet for CAN_BUS_IDLE_CLOSE_MS and every open
// file has been flushed, closed, digested and renamed to .log.
//
// ⭐ This is the key-off guarantee. The transceiver's INH pin removes power
// with no warning when the bus sleeps, so a trip that ends cleanly must have
// closed its files BEFORE that happens -- the periodic flush is only the
// backstop for a crash, never the mechanism for a normal key-off.
bool filestoreIdleClosed();

// Milliseconds since the last received frame. Feeds the go-to-sleep
// invariant in sleepguard.h. Returns 0 if no frame has ever arrived, so a
// board that has never seen the bus never looks 'quiet enough' to sleep.
uint32_t filestoreBusQuietMs();

const FileStoreStats *filestoreStats();

// Percent of the partition in use, 0-100.
uint8_t filestoreUsagePct();

// The storage warning the hub reports and the dashboard shows. THE ONE PLACE
// THE WARN RULE LIVES -- it was computed inline at the API before, which is
// how it came to mean only "total usage is high".
//
// 🔑 Retention run 2 (2026-09-23) measured the gap: three unacked Tier A files,
// 197,285 bytes, were destroyed at 55% usage with `warn` still false, because
// enforceTierACap() evicts at 40% of the PARTITION while the warning fires at
// 70% of TOTAL. Evicting unacked data at the cap is allowed; doing it silently
// is not. So warn is true if EITHER the partition is filling OR data has been
// lost that the hub has not yet recorded.
//
// ⭐ The second arm is `fsLostFilesTotal() > lostAckedFiles`, and BOTH sides of
// that comparison live in NVS. A latched RAM flag would have been erased by
// the key-off that ends every trip -- the same power cut that erases the
// since-boot counters. Deriving the warning from two PERSISTENT numbers means
// it survives until the hub actually writes the loss down.
bool filestoreWarn();

// Records that the hub has durably stored the loss record up to this many
// lifetime lost files (protocol 2.3). A watermark: idempotent, clamped to the
// real total, and it never moves backwards -- a hub that lost its own state
// must not be able to silence a warning it never recorded. Returns the
// watermark in force afterwards.
uint32_t filestoreAckLoss(uint32_t throughFiles);

// Registers /api/v1/files, /api/v1/files/<index> and /api/v1/files/ack, and
// the two safe-mode endpoints, POST /api/v1/filestore/erase and
// POST /api/v1/filestore/retry (both X-Hub-Token, both 409 outside safe mode).
void filestoreRegister(WebServer &srv);

// Applies a watermark ack (protocol §2.1). Idempotent, never moves backwards.
// Returns the watermark actually in force afterwards.
int32_t filestoreAck(int32_t throughIndex);

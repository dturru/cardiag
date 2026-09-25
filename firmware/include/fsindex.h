#pragma once

// ---------------------------------------------------------------------------
// The filestore's INDEX and RETENTION POLICY, with no I/O.
//
// Split out of filestore.cpp so the two properties that failed on hardware can
// be tested on the host:
//
//   1. EVERY FILE ON DISK IS IN THE INDEX. The old table was a fixed array of
//      FS_MAX_FILES (96) and addEntry() returned nullptr when it was full;
//      scanDir() then did `continue`. Measured 2026-09-25 on the soak image:
//      1,376 directory entries, 594 files dropped from the index. A file that
//      is not in the index is invisible to evictOne() and enforceRetention(),
//      so it can never be evicted, and the directory only grows.
//
//   2. THE FILE COUNT IS BOUNDED, not just the bytes. Retention only ever
//      looked at bytes. 686 ~5 KB files are 3.4 MB -- inside the byte limits
//      -- and every one of them costs a directory entry that LittleFS walks on
//      each path lookup, which is what made the boot scan quadratic.
//
// The table GROWS (the allocator is injected: PSRAM on the board, malloc on
// the host). If growth fails, the oldest entry is evicted to make room -- with
// the same loss accounting as any other unacked eviction -- rather than the
// newcomer being dropped silently. Silently is the part that is not allowed.
//
// Nothing here touches LittleFS, NVS, Serial or the watchdog. The caller
// supplies those as callbacks, so the host test can run the REAL policy
// against a fake disk instead of a second copy of it.
// ---------------------------------------------------------------------------

#include <stdint.h>
#include <stddef.h>

#include "filestore.h"   // tier/kind letters and fsTierForKind(); no I/O

// What the name says about a file. Everything the boot scan needs comes from
// here -- no open, no stat. That is what makes the scan linear.
enum FsExt : uint8_t {
  FS_EXT_NONE = 0,   // not one of ours
  FS_EXT_LOG  = 1,   // closed, complete
  FS_EXT_PART = 2,   // being written, or a crash artifact from an older boot
  FS_EXT_META = 3,   // digest sidecar for a .log
};

struct FsName {
  uint32_t index;
  uint32_t bootId;
  char     kind;
  char     tierInName;   // as written; the real tier is fsTierForKind(kind)
  FsExt    ext;
};

// Parses NNNNNN_TK_BBBBBBBB.ext. Returns false for anything that is not
// exactly that shape, including a known shape with an unknown kind letter.
bool fsParseName(const char *base, FsName *out);

struct FsEntry {
  uint32_t index;
  uint32_t bytes;       // valid only when `hydrated`
  uint32_t bootId;
  uint8_t  sha[32];
  char     kind;        // tier is fsTierForKind(kind); never stored twice
  uint8_t  mode;
  bool     closed;      // .log (true) or .part (false)
  bool     hasSha;
  // Being written by THIS boot. The only files retention may never touch.
  // A .part left by an earlier boot is NOT active: it is a crash artifact
  // nobody will ever close, and it must be evictable or a board that resets
  // every drive accumulates them forever.
  bool     active;
  // bytes/sha/mode have been read from disk. The boot scan fills in only what
  // the NAME says; the rest is read lazily (filestore.cpp, hydrate), because
  // each read is a LittleFS path lookup and a path lookup walks the directory.
  bool     hydrated;
};

// Realloc-shaped. May return nullptr; the table then evicts instead of
// dropping. `oldBytes` is passed so an allocator without realloc can copy.
typedef void *(*FsGrowFn)(void *old, size_t oldBytes, size_t newBytes);

struct FsTable {
  FsEntry *v;
  uint16_t count;
  uint16_t cap;
  FsGrowFn grow;
};

// Hard ceiling on the table, so the uint16_t count can never wrap. Far above
// anything a 3.7 MB partition can hold in real files (the soak image had 688).
#define FS_TABLE_HARD_MAX 60000u

void     fsTableInit(FsTable *t, FsGrowFn grow);
FsEntry *fsTableFind(FsTable *t, uint32_t index);
// Returns the entry for `index`, adding it if absent. nullptr only when the
// table is full AND growth failed -- see fsTableAddOrEvict for the scan path.
FsEntry *fsTableAdd(FsTable *t, uint32_t index);
void     fsTableSort(FsTable *t);      // ascending index
void     fsTableDrop(FsTable *t, uint16_t i);

// The scan's add: never drops. If the table cannot grow, the OLDEST
// non-active entry loses its slot -- the older of it and the newcomer, so the
// rule is oldest-first whatever order readdir() returns names in.
//
// `incoming` is the new entry, already filled in from its name. Returns the
// slot it now occupies, or nullptr if the newcomer itself was the oldest.
// `*didEvict` is set whenever something lost its slot, and `*evicted` then
// holds that entry in full (the newcomer, if it was the victim) -- the caller
// deletes it from disk and accounts for it like any other eviction.
FsEntry *fsTableAddOrEvict(FsTable *t, const FsEntry *incoming,
                           FsEntry *evicted, bool *didEvict);

// ---------------------------------------------------------------------------
// Boot scan
//
// One call per directory entry, NAME ONLY. The caller does readdir() and
// nothing else in the loop -- no open, no stat. Every other fact about a file
// (size, digest, mode) is read later and lazily, because on LittleFS each path
// lookup walks the directory: n lookups over an n-entry directory is the
// quadratic that took 174 s on 1,376 entries.
// ---------------------------------------------------------------------------

struct FsScanStats {
  uint32_t entries;          // everything readdir() returned
  uint32_t files;            // .log + .part, i.e. what the index should hold
  uint32_t metas;
  uint32_t unparsed;         // names that are not ours -- reported, not deleted
  uint32_t duplicates;       // a .log and a .part with the same index
  uint32_t evictedForRoom;   // table full and could not grow
};

enum FsScanResult : uint8_t {
  FS_SCAN_SKIP  = 0,   // .meta, or not ours
  FS_SCAN_ADDED = 1,   // in the index
  FS_SCAN_EVICT = 2,   // something lost its slot; *evicted says what
};

FsScanResult fsScanFeed(FsTable *t, const char *base, FsScanStats *st,
                        FsEntry *evicted);

// ---------------------------------------------------------------------------
// Retention
// ---------------------------------------------------------------------------

struct FsRetentionCfg {
  uint32_t maxFiles;        // count cap (FS_MAX_FILES)
  uint32_t tierACapBytes;   // Tier A share of the partition
  uint32_t usedLimitBytes;  // reclaim above this (90% of the partition)
};

struct FsRetentionOps {
  void *ctx;
  // Read bytes/sha/mode for an entry whose `hydrated` is false. Called before
  // an entry is evicted, so the loss record carries its real byte count.
  void (*hydrate)(void *ctx, FsEntry *e);
  // Delete the file(s) from disk and do the accounting: acked -> a free
  // deletion; unacked -> the NVS loss record and the loud warning. The table
  // entry is dropped by the policy afterwards.
  void (*remove)(void *ctx, const FsEntry *e, bool acked);
  // Real bytes in use on the partition (LittleFS.usedBytes() on the board).
  uint32_t (*usedBytes)(void *ctx);
};

enum FsEvictReason : uint8_t {
  FS_EVICT_NONE = 0,
  FS_EVICT_COUNT = 1,     // over the file-count cap
  FS_EVICT_TIER_A = 2,    // Tier A over its share
  FS_EVICT_USED = 3,      // partition above the reclaim limit
};

struct FsRetentionResult {
  uint16_t evicted;
  uint16_t evictedAcked;
  uint16_t evictedUnacked;
  // True when the per-call budget ran out with work left. The caller runs
  // retention again on the next pass instead of waiting for the timer, so a
  // backlog drains in seconds without any single call blocking loop().
  bool     more;
  // True when a limit is exceeded but nothing is evictable (only active
  // files remain). Not a loop: the policy stops and says so.
  bool     stuck;
};

// Sum of hydrated Tier A bytes. Unhydrated entries count as zero until read,
// which can only make the Tier A cap fire LATER, never evict early.
uint32_t fsTierABytes(const FsTable *t);

// Protocol 2.3 deletion order, as a pure choice:
//   1. acked, oldest first
//   2. unacked Tier A, oldest first
//   3. unacked Tier B
//   4. unacked Tier C
// Active files are never chosen. Returns t->count when nothing is evictable.
uint16_t fsPickVictim(const FsTable *t, int32_t ackedThrough);

// Oldest Tier A file, acked ones first. Returns t->count when none.
uint16_t fsPickTierAVictim(const FsTable *t, int32_t ackedThrough);

// Enforces, in order: the count cap (leaving `reserve` free slots for files
// about to be opened), the Tier A cap, and the used-bytes limit. Evicts at
// most `budget` files per call.
FsRetentionResult fsEnforceRetention(FsTable *t, int32_t ackedThrough,
                                     const FsRetentionCfg *cfg,
                                     const FsRetentionOps *ops,
                                     uint16_t reserve, uint16_t budget);

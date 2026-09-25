#pragma once

// ---------------------------------------------------------------------------
// LittleFS usage, KEPT IN RAM instead of asked of the filesystem.
//
// WHY (2026-09-25). LittleFS.usedBytes() AND LittleFS.totalBytes() both go
// through esp_littlefs_info(), which computes used bytes with lfs_fs_size(): a
// traversal of every block the filesystem owns. Retention asked for them at
// least five times per run (recomputeUsage() twice with both, usableBytes(),
// and retUsed() per policy check), on the 5 s timer, on every file open and
// on every close; filestoreUsagePct() asked twice more on each session read.
// The master soak had a 1.2-2.4 s filestore pass on every one of 40 cycles.
//
// Now the filesystem is measured ONCE, at boot, after the scan. From then on
// every byte this code adds or removes is accounted here, in data blocks:
// that is what a file actually costs LittleFS.
//
//   * A file at or under FS_INLINE_MAX bytes lives inline in its directory's
//     metadata and costs no data block (the .meta sidecars, empty .parts).
//   * Larger files cost ceil(bytes / block) blocks. LittleFS's CTZ skip-list
//     pointers add a few bytes per block; ignored (under 0.2 % at 4 KB).
//   * Directory metadata is in the boot measurement and grows little: the
//     file-count cap bounds the entries.
//
// The error is therefore small and one-sided per file, and retention's 90 %
// reclaim threshold is the margin it has always had. Header-only, stdint
// only: test/test_fsusage.
// ---------------------------------------------------------------------------

#include <stdint.h>

struct FsUsage {
  uint32_t blockBytes;   // LittleFS block size (4096 on the ESP32 port)
  uint32_t inlineMax;    // files this small take no data block
  uint32_t totalBytes;   // measured at boot
  uint32_t usedBytes;    // measured at boot, then kept by the deltas below
  bool     live;         // false until the boot measurement is in
};

static inline uint32_t fsDataBlocks(const FsUsage *u, uint32_t bytes) {
  if (bytes <= u->inlineMax || !u->blockBytes) return 0;
  return (uint32_t)(((uint64_t)bytes + u->blockBytes - 1) / u->blockBytes);
}

static inline void fsUsageMeasured(FsUsage *u, uint32_t total, uint32_t used) {
  u->totalBytes = total;
  u->usedBytes = used;
  u->live = true;
}

// A file went from `oldBytes` to `newBytes` (a write, or a remove: newBytes=0).
static inline void fsUsageResize(FsUsage *u, uint32_t oldBytes,
                                 uint32_t newBytes) {
  if (!u->live) return;   // before the boot measurement: it will include this
  const uint32_t a = fsDataBlocks(u, oldBytes), b = fsDataBlocks(u, newBytes);
  if (b >= a) {
    const uint64_t add = (uint64_t)(b - a) * u->blockBytes;
    const uint64_t n = (uint64_t)u->usedBytes + add;
    u->usedBytes = n > u->totalBytes ? u->totalBytes : (uint32_t)n;
  } else {
    const uint64_t sub = (uint64_t)(a - b) * u->blockBytes;
    u->usedBytes = sub > u->usedBytes ? 0 : u->usedBytes - (uint32_t)sub;
  }
}

static inline uint8_t fsUsagePct(const FsUsage *u) {
  if (!u->totalBytes) return 0;
  return (uint8_t)((uint64_t)u->usedBytes * 100 / u->totalBytes);
}

#include <Arduino.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <Preferences.h>
#include <uri/UriBraces.h>
#include <mbedtls/sha256.h>
// Retention and file downloads are legitimate long operations that run inside
// loop()'s watchdog window, so they feed it after each unit of progress. See
// the esp_task_wdt_reset() calls below; each one documents what bought it.
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>

// Feed the task watchdog ONLY if this task is actually subscribed to it.
//
// 🐛 WHY THE GUARD (found on the bench 2026-09-24): calling
// esp_task_wdt_reset() from a task that is not subscribed prints
// `E task_wdt: esp_task_wdt_reset(707): task not found`. Harmless, but a
// benign error printed every boot is the noise that hides a real one.
//
// ⚠️ NOT CALLED ANYWHERE UNDER filestoreBegin(). Since 2026-09-25 the watchdog
// is armed BEFORE filestoreBegin() with the boot budget (FS_BOOT_WDT_S), and
// that budget is the bound on the whole start: feeding it from inside would
// let a slow-but-progressing scan run forever, which is the 187 s boot this
// replaced. Feeds are for loop()-time work that is bought with progress --
// retention, hydration, downloads.
static inline void wdtFeedIfArmed() {
  if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
}

#include "filestore.h"
#include "busidle.h"
#include "fsprof.h"
#include <atomic>
#include "fsindex.h"
#include "bootguard_rt.h"
#include "recorder.h"
#include "sniffer.h"
#include "session.h"
#include "config.h"
#include "secrets.h"

// ---------------------------------------------------------------------------
// ON-DISK LAYOUT
//
//   /log/NNNNNN_TK_BBBBBBBB.part    being written right now
//   /log/NNNNNN_TK_BBBBBBBB.log     closed, complete
//   /log/NNNNNN_TK_BBBBBBBB.meta    sha256 + byte count for the .log
//
// NNNNNN   index, a monotonic NVS counter. The hub's watermark is an index, so
//          it must never be reused and must sort chronologically as text.
// T        TIER letter -- the retention class: A raw/changes, B snapshot,
//          C bookend. This is what the deletion order sorts on.
// K        KIND letter -- what is in the file: r raw, c changes, s snapshot,
//          b bookend. Tier A holds TWO kinds, which is why one letter was not
//          enough and why the earlier single-letter scheme drifted.
// BBBBBBBB boot_id in hex. This is what lets an interrupted write be
//          identified as a CRASH ARTIFACT rather than a file in progress: a
//          .part whose boot_id is not the current one will never be closed by
//          anybody, and the hub already knows to sync it as truncated.
//
// CLOSE ORDER IS LOAD-BEARING: flush -> write .meta -> rename .part to .log.
// A .log therefore always has its digest. The reverse order would produce
// files that exist, look closed, and cannot be verified.
// ---------------------------------------------------------------------------

// Tier B block: u32 ms, u16 count, then count * 13 bytes of {u32 id, u8 dlc,
// u8 data[8]}. No decoding, no car knowledge -- the id is a number here.
#define SNAP_REC_LEN 13

// Tier B file header, so the binary is self-describing rather than needing the
// reader to already know what it is holding.
struct __attribute__((packed)) SnapFileHeader {
  char     magic[4];     // "CDGS"
  uint8_t  version;      // 1
  uint8_t  recLen;       // SNAP_REC_LEN
  uint8_t  mode;         // logger mode when written (see config.h MODE_*)
  uint8_t  reserved;
  uint32_t deviceId;
  uint32_t bootId;
};
static_assert(sizeof(SnapFileHeader) == 16, "SnapFileHeader must be 16 bytes");

// The index. GROWS -- see fsindex.h for why a fixed FS_MAX_FILES array was
// the bug. FS_MAX_FILES is now retention's file-count cap, not a table size.
typedef FsEntry FileEntry;
static FsTable g_tab;
static FileStoreStats g_st;

// Set when a retention pass ran out of budget with work left, so the next
// loop() pass continues instead of waiting for the 5 s timer.
static bool g_retentionMore = false;
// Next entry to look at when hydrating; wraps.
static uint16_t g_hydrateCursor = 0;

static Preferences g_fsPrefs;
static bool  g_enabled = false;
static uint8_t g_mode  = 0xFF;

// Active writers, one per KIND being written. Tier A and Tier B are open at
// the same time, so these cannot be one writer.
struct Active {
  File     fh;
  uint32_t index;
  uint32_t bytes;
  uint32_t lastFlushMs;   // bounds what a crash can cost; see flushDue()
  char     kind;
  bool     open;
  mbedtls_sha256_context sha;
};
static Active g_actSnapshot;   // Tier B
static Active g_actChanges;    // Tier A

static uint32_t g_nextSnapMs = 0;

// Bus-idle tracking for the clean key-off close. Written by canTask on every
// received frame (the other core), read by loop(). Atomic, and read ONCE per
// comparison: canTask can store a timestamp NEWER than a `now` loop() sampled
// earlier in the same pass. See busidle.h for the 49.7-day wrap that caused.
static std::atomic<uint32_t> g_lastBusMs{0};
// Cleared by canTask on traffic, set by loop() on the idle close.
static std::atomic<bool> g_idleClosed{false};
static RecCsvCursor g_chgCursor = {0, false};

// Sub-stage profile of the tick (fsprof.h). Only filestoreLoop() opens a
// pass, so the same code reached from a web handler is not counted here.
static FsProf g_prof = {};
static FsSubWindow g_subWin[2] = {};    // [0] session, [1] stats line
static FsSubWindow g_subBoot = {};      // since boot, never reset

struct FsSubScope {
  explicit FsSubScope(uint8_t s) { fsProfEnter(&g_prof, s, micros()); }
  ~FsSubScope() { fsProfExit(&g_prof, micros()); }
};
#define FS_SUB(s) FsSubScope fsSubScope_(s)

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

static void makeName(char *out, size_t cap, uint32_t index, char kind,
                     uint32_t bootId, const char *ext) {
  snprintf(out, cap, FS_DIR "/%06lu_%c%c_%08lX.%s",
           (unsigned long)index, fsTierForKind(kind), kind,
           (unsigned long)bootId, ext);
}

// Full VFS path for a file, for the POSIX calls. LittleFS is mounted at
// FS_VFS_ROOT (config.h) explicitly, so this cannot drift from the mount.
static void makeVfsName(char *out, size_t cap, uint32_t index, char kind,
                        uint32_t bootId, const char *ext) {
  char rel[64];
  makeName(rel, sizeof(rel), index, kind, bootId, ext);
  snprintf(out, cap, "%s%s", FS_VFS_ROOT, rel);
}

// ---------------------------------------------------------------------------
// Index table -- thin wrappers over fsindex.cpp
// ---------------------------------------------------------------------------

// PSRAM first: the table is ~52 B per file and the soak image had 690 files.
// Internal RAM only as a fallback, and nullptr is a real answer -- the scan
// then evicts oldest-first with loss accounting instead of dropping.
static void *growInPsram(void *old, size_t, size_t newBytes) {
  void *p = heap_caps_realloc(old, newBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_realloc(old, newBytes, MALLOC_CAP_8BIT);
  return p;
}

static FileEntry *findEntry(uint32_t index) { return fsTableFind(&g_tab, index); }
static void sortEntries() { fsTableSort(&g_tab); }
static void dropEntry(uint16_t i) { fsTableDrop(&g_tab, i); }

// `withFsTotals` = also ask LittleFS for used/total bytes. That call walks
// the whole filesystem (lfs_fs_size), so the per-pass hydration skips it: it
// only changes the per-tier sums, never what is on disk.
static void recomputeUsage(bool withFsTotals = true) {
  g_st.files = g_tab.count;
  g_st.openFiles = 0;
  g_st.pendingUnacked = 0;
  g_st.unhydrated = 0;
  g_st.tierABytes = g_st.tierBBytes = g_st.tierCBytes = 0;
  for (uint16_t i = 0; i < g_tab.count; i++) {
    const FileEntry &e = g_tab.v[i];
    if (!e.hydrated) g_st.unhydrated++;
    if (!e.closed) g_st.openFiles++;
    // Closed and above the watermark: finished, and the hub does not have it.
    // Only the logger can count this -- the hub cannot derive it from files,
    // open and acked_through, because evictions punch holes in the index
    // range. Between trips this is normally non-zero; see protocol 2.3.1.
    else if ((int32_t)e.index > g_st.ackedThrough) g_st.pendingUnacked++;
    // Byte totals count only what has been read. Partial until the index is
    // hydrated; they can only grow as it fills in, never overstate.
    if (!e.hydrated) continue;
    switch (fsTierForKind(e.kind)) {
      case FS_TIER_SNAPSHOT: g_st.tierBBytes += e.bytes; break;
      case FS_TIER_BOOKEND:  g_st.tierCBytes += e.bytes; break;
      default:               g_st.tierABytes += e.bytes; break;
    }
  }
  if (!withFsTotals) return;
  FS_SUB(FS_SUB_FSSIZE);
  g_st.usedBytes = LittleFS.usedBytes();
  g_st.totalBytes = LittleFS.totalBytes();
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

// Recover the producing mode from the FILE ITSELF, for a file that has no
// .meta yet.
//
// 🐛 WHY THIS EXISTS (found on hardware 2026-09-23). A closed file round-trips
// its mode through `mode=` in the .meta sidecar. An OPEN .part has no .meta,
// so after a reboot its mode came back unknown -- and the listing rendered
// unknown as `"synthetic":false`. A SELFTEST run interrupted by a reset (which
// is every run, because closing the USB serial port resets this board) was
// therefore served to the hub as REAL CAR DATA. That is precisely the failure
// the synthetic flag exists to prevent, and it was silent.
//
// The Tier B header already carries the mode, so the fact is on the disk; it
// simply was not being read back. Content wins over the label, as everywhere
// else in this project.
static uint8_t readModeFromContent(const FileEntry &e) {
  // Only Tier B is self-describing. A CSV has a column header and no room for
  // provenance, so for those kinds the mode is genuinely unrecoverable and
  // must stay UNKNOWN rather than being guessed at.
  if (e.kind != FS_KIND_SNAPSHOT) return FS_MODE_UNKNOWN;

  char path[80];
  makeVfsName(path, sizeof(path), e.index, e.kind, e.bootId, "part");
  FILE *f = fopen(path, "rb");
  if (!f) return FS_MODE_UNKNOWN;
  SnapFileHeader h{};
  const size_t n = fread(&h, 1, sizeof(h), f);
  fclose(f);
  if (n != sizeof(h) || memcmp(h.magic, "CDGS", 4) != 0) return FS_MODE_UNKNOWN;
  return h.mode;
}

// sha256 / mode / bytes from the .meta sidecar. Returns true if it held a
// byte count, so the caller only stats the .log when it has to.
static bool readMeta(FileEntry *e) {
  char path[80];
  makeVfsName(path, sizeof(path), e->index, e->kind, e->bootId, "meta");
  FILE *f = fopen(path, "r");
  if (!f) return false;
  bool haveBytes = false;
  char line[128];
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = 0;
    if (strncmp(line, "sha256=", 7) == 0 && strlen(line + 7) >= 64) {
      for (int i = 0; i < 32; i++) {
        char b[3] = {line[7 + i * 2], line[8 + i * 2], 0};
        e->sha[i] = (uint8_t)strtoul(b, nullptr, 16);
      }
      e->hasSha = true;
    } else if (strncmp(line, "mode=", 5) == 0) {
      e->mode = (uint8_t)atoi(line + 5);
    } else if (strncmp(line, "bytes=", 6) == 0) {
      e->bytes = (uint32_t)strtoul(line + 6, nullptr, 10);
      haveBytes = true;
    }
  }
  fclose(f);
  return haveBytes;
}

// Fills in what the NAME does not say: size, digest, mode.
//
// ⭐ THIS IS WHERE THE PATH LOOKUPS WENT. Each open or stat on LittleFS walks
// the directory to find the name, so doing this for every file inside the
// boot scan made the scan quadratic: 1,376 entries, 174 s, measured
// 2026-09-25. It now runs lazily -- a few entries per loop() pass, on demand
// in the listing, and for an eviction victim just before it is counted --
// so no single call does n of them.
//
// A closed file costs one lookup (its .meta, which also carries the byte
// count). A .part costs a stat, plus one read of the header for Tier B.
static void hydrate(FileEntry *e) {
  if (e->hydrated) return;
  FS_SUB(FS_SUB_HYDRATE);
  e->mode = FS_MODE_UNKNOWN;
  bool haveBytes = false;
  if (e->closed) haveBytes = readMeta(e);
  if (!haveBytes) {
    char path[80];
    makeVfsName(path, sizeof(path), e->index, e->kind, e->bootId,
                e->closed ? "log" : "part");
    struct stat st;
    e->bytes = (stat(path, &st) == 0) ? (uint32_t)st.st_size : 0;
  }
  if (!e->closed) e->mode = readModeFromContent(*e);
  e->hydrated = true;
}

// Declared here, defined with retention: a scan-time eviction goes through
// the same accounting as every other one.
static void removeFiles(const FileEntry &e);
static void noteUnackedEviction(const FileEntry &e);

// Rebuilds the index from the directory NAMES. No open, no stat.
//
// 🐛 THE OLD SCAN (measured 2026-09-25 on the 200-cycle soak image):
//   * File::openNextFile() constructs a VFSFileImpl for every entry, which
//     stats and opens it (vfs_api.cpp:481) -- a path lookup per entry, and
//     each lookup walks the directory. readMeta() then opened the .meta too.
//     1,376 entries took 174 s, k ~ 0.1 ms/entry^2.
//   * The index was a fixed FS_MAX_FILES (96) array and a full table meant
//     `continue`: 594 files dropped, invisible to retention forever.
//
// Now: readdir() over the VFS path returns names without opening anything,
// fsScanFeed() classifies each one from its name, and the table grows. If it
// cannot grow, the oldest file is evicted WITH loss accounting -- never
// dropped. That eviction removes a file while the directory is open; LittleFS
// is designed to fix up open directory handles on commit, but this path is
// only reached when PSRAM refuses to grow the table and has NOT been
// exercised on hardware.
static void scanDir() {
  fsTableInit(&g_tab, growInPsram);
  g_hydrateCursor = 0;

  char dirPath[48];
  snprintf(dirPath, sizeof(dirPath), "%s%s", FS_VFS_ROOT, FS_DIR);
  DIR *d = opendir(dirPath);
  if (!d) {
    LittleFS.mkdir(FS_DIR);
    recomputeUsage();
    return;
  }

  FsScanStats st;
  memset(&st, 0, sizeof(st));
  struct dirent *de;
  while ((de = readdir(d)) != nullptr) {
    if (de->d_type == DT_DIR) continue;
    FileEntry ev;
    if (fsScanFeed(&g_tab, de->d_name, &st, &ev) != FS_SCAN_EVICT) continue;
    // Table full and PSRAM refused to grow it. Oldest-first, and never
    // silent: read its size so the loss record is exact, delete, account.
    hydrate(&ev);
    removeFiles(ev);
    if ((int32_t)ev.index <= g_st.ackedThrough) g_st.deletedAcked++;
    else noteUnackedEviction(ev);
  }
  closedir(d);

  g_st.scanEntries = st.entries;
  g_st.scanFiles = st.files;
  g_st.scanForeign = st.unparsed;
  g_st.scanEvictedForRoom = st.evictedForRoom;
  if (st.unparsed) {
    Serial.printf("[fs] %lu name(s) in %s are not ours; left alone\n",
                  (unsigned long)st.unparsed, FS_DIR);
  }
  if (st.evictedForRoom) {
    Serial.printf("[fs] *** index could not grow: %lu file(s) evicted "
                  "oldest-first to fit (each counted above) ***\n",
                  (unsigned long)st.evictedForRoom);
  }
  sortEntries();
  recomputeUsage();
}

// A few entries per call, so hydration never holds loop() for long. Returns
// true once every entry is hydrated.
static bool hydrateSome(uint16_t budget) {
  if (!g_st.unhydrated) return true;
  uint16_t done = 0;
  for (uint16_t seen = 0; seen < g_tab.count && done < budget; seen++) {
    if (g_hydrateCursor >= g_tab.count) g_hydrateCursor = 0;
    FileEntry *e = &g_tab.v[g_hydrateCursor++];
    if (e->hydrated) continue;
    hydrate(e);
    done++;
    wdtFeedIfArmed();         // bought with progress: one entry read
  }
  if (done) recomputeUsage(/*withFsTotals=*/false);
  if (!g_st.unhydrated) {
    Serial.printf("[fs] index hydrated: %u files, tier A %lu B, tier B %lu B\n",
                  g_tab.count, (unsigned long)g_st.tierABytes,
                  (unsigned long)g_st.tierBBytes);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Retention (protocol 2.3)
// ---------------------------------------------------------------------------

static void removeFiles(const FileEntry &e) {
  char path[64];
  makeName(path, sizeof(path), e.index, e.kind, e.bootId, e.closed ? "log" : "part");
  LittleFS.remove(path);
  makeName(path, sizeof(path), e.index, e.kind, e.bootId, "meta");
  LittleFS.remove(path);
}

// The ONE place an unacked deletion is accounted for. Both eviction paths go
// through it, because the gap retention run 2 found was exactly that the cap
// path did its own bookkeeping and skipped the warning.
//
// Call BEFORE dropEntry(): the entry still holds the byte count.
static void noteUnackedEviction(const FileEntry &e) {
  const char tier = fsTierForKind(e.kind);
  const uint8_t slot = fsTierSlot(tier);
  g_st.deletedUnacked++;
  g_st.lostBytes[slot] += e.bytes;
  g_st.lostFiles[slot]++;

  // ⭐ PERSIST IMMEDIATELY, and only the tier that changed -- two keys, not
  // six. This is the write that makes the loss survive key-off, so it has to
  // happen here rather than at some tidier later point: INH can remove power
  // between this eviction and the next loop() pass.
  //
  // On wear: a write happens ONLY when unacked data is destroyed. That is
  // supposed to be rare, and if it is frequent enough for NVS wear to matter
  // then the device is destroying uncollected data continuously, which is the
  // bug to fix rather than a reason to stop recording it.
  char key[8];
  snprintf(key, sizeof(key), "lf%u", (unsigned)slot);
  g_fsPrefs.putULong(key, g_st.lostFiles[slot]);
  snprintf(key, sizeof(key), "lb%u", (unsigned)slot);
  g_fsPrefs.putULong64(key, g_st.lostBytes[slot]);
  // Loud on purpose. This line is the only trace a field unit leaves of data
  // the hub will now never receive.
  Serial.printf("[fs] *** EVICTED UNACKED TIER %c (%s) #%lu, %lu B -- DATA LOST "
                "(tier %c LIFETIME now %lu files / %llu B; hub has recorded "
                "%lu of %lu) ***\n",
                tier, fsKindName(e.kind), (unsigned long)e.index,
                (unsigned long)e.bytes, tier,
                (unsigned long)g_st.lostFiles[slot],
                (unsigned long long)g_st.lostBytes[slot],
                (unsigned long)g_st.lostAckedFiles,
                (unsigned long)fsLostFilesTotal(&g_st));
}

static uint32_t usableBytes() {
  const uint32_t total = LittleFS.totalBytes();
  return total ? total : (uint32_t)HUB_FS_USABLE_BYTES;
}

// --- the callbacks fsEnforceRetention() drives -------------------------------

static void retHydrate(void *, FsEntry *e) { hydrate(e); }

// Deletes and accounts. The ONE place retention's side effects live, so the
// count cap, the Tier A cap and the reclaim all report a loss the same way --
// the gap retention run 2 found was one path doing its own bookkeeping.
static void retRemove(void *, const FsEntry *e, bool acked) {
  if (acked) {
    Serial.printf("[fs] evict acked #%lu\n", (unsigned long)e->index);
    removeFiles(*e);
    g_st.deletedAcked++;
  } else {
    removeFiles(*e);
    noteUnackedEviction(*e);
  }
  // Bought with progress: one file is provably gone. The per-call budget
  // bounds how many of these a single pass can do.
  wdtFeedIfArmed();
}

static uint32_t retUsed(void *) {
  FS_SUB(FS_SUB_FSSIZE);
  return LittleFS.usedBytes();
}

// Protocol 2.3, plus the file-count cap. `reserve` keeps slots free for files
// about to be opened. Evicts at most FS_EVICT_PER_PASS files per call; a
// backlog (the soak image: ~600 files over the cap) drains across loop()
// passes instead of in one call that holds the web server and the UDP stream.
static void enforceRetention(uint16_t reserve = 0) {
  FS_SUB(FS_SUB_RETENTION);
  recomputeUsage();
  const uint32_t total = usableBytes();
  const FsRetentionCfg cfg = {
      FS_MAX_FILES,
      (uint32_t)((uint64_t)total * FS_TIER_A_MAX_PCT / 100),
      // LittleFS needs slack to do anything at all, including delete.
      // Reclaim before it is full rather than at the moment a write fails.
      (uint32_t)((uint64_t)total * 90 / 100),
  };
  const FsRetentionOps ops = {nullptr, retHydrate, retRemove, retUsed};
  const FsRetentionResult r = fsEnforceRetention(
      &g_tab, g_st.ackedThrough, &cfg, &ops, reserve, FS_EVICT_PER_PASS);
  g_retentionMore = r.more;
  if (r.stuck) {
    // Over a limit with nothing evictable: only files being written right
    // now are left. Not a loop -- say so and let the next close free one.
    static uint32_t lastSaid = 0;
    if (millis() - lastSaid > 60000) {
      lastSaid = millis();
      Serial.printf("[fs] retention: over a limit with only active files "
                    "left (%u files)\n", g_tab.count);
    }
  }
  recomputeUsage();
}

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------

static uint32_t nextIndex() {
  const uint32_t idx = g_fsPrefs.getULong("idx", 0);
  g_fsPrefs.putULong("idx", idx + 1);
  g_st.nextIndex = idx + 1;
  return idx;
}

// Runtime add. The table grows; if it cannot, the oldest evictable file
// gives up its slot WITH the usual accounting -- a file on disk that the index
// does not know about is the bug this replaced, so there is no path to one.
// nullptr only if every other entry is active, which two writers cannot
// produce against a cap of FS_MAX_FILES.
static FileEntry *trackEntry(const FileEntry &in) {
  FS_SUB(FS_SUB_ROTATE);
  FileEntry ev;
  bool did = false;
  FileEntry *e = fsTableAddOrEvict(&g_tab, &in, &ev, &did);
  if (did && ev.index != in.index) {
    hydrate(&ev);
    removeFiles(ev);
    if ((int32_t)ev.index <= g_st.ackedThrough) g_st.deletedAcked++;
    else noteUnackedEviction(ev);
  }
  return e;
}

static void closeActive(Active &a) {
  if (!a.open) return;
  FS_SUB(FS_SUB_ROTATE);
  a.fh.flush();
  a.fh.close();
  a.open = false;

  uint8_t digest[32];
  mbedtls_sha256_finish(&a.sha, digest);
  mbedtls_sha256_free(&a.sha);

  const uint32_t boot = sessionBootId();
  char metaPath[64], partPath[64], logPath[64];
  makeName(metaPath, sizeof(metaPath), a.index, a.kind, boot, "meta");
  makeName(partPath, sizeof(partPath), a.index, a.kind, boot, "part");
  makeName(logPath, sizeof(logPath), a.index, a.kind, boot, "log");

  // Digest first, rename last: a .log without a .meta must be impossible.
  File m = LittleFS.open(metaPath, "w");
  if (m) {
    m.print("sha256=");
    for (int i = 0; i < 32; i++) m.printf("%02x", digest[i]);
    m.printf("\nbytes=%lu\nmode=%u\n", (unsigned long)a.bytes, (unsigned)g_mode);
    m.close();
  } else {
    g_st.writeErrors++;
  }

  if (!LittleFS.rename(partPath, logPath)) {
    g_st.writeErrors++;
    Serial.printf("[fs] rename failed for #%lu\n", (unsigned long)a.index);
  }

  FileEntry in;
  memset(&in, 0, sizeof(in));
  in.index = a.index;
  in.kind = a.kind;
  in.bytes = a.bytes;
  in.bootId = boot;
  in.closed = true;
  in.mode = g_mode;
  in.hasSha = true;
  memcpy(in.sha, digest, 32);
  in.active = false;       // closed: retention may take it from now on
  in.hydrated = true;      // everything is known; nothing to read back
  trackEntry(in);
  sortEntries();
  recomputeUsage();
  Serial.printf("[fs] closed #%lu tier %c kind %s, %lu B\n",
                (unsigned long)a.index, fsTierForKind(a.kind),
                fsKindName(a.kind), (unsigned long)a.bytes);
}

static bool openActive(Active &a, char kind) {
  FS_SUB(FS_SUB_ROTATE);
  // Reserve one slot, so the file about to be opened cannot take the count
  // back over FS_MAX_FILES.
  enforceRetention(/*reserve=*/1);

  a.index = nextIndex();
  a.kind = kind;
  a.bytes = 0;
  a.lastFlushMs = millis();
  mbedtls_sha256_init(&a.sha);
  mbedtls_sha256_starts(&a.sha, 0);   // 0 = SHA-256, not SHA-224

  char path[64];
  makeName(path, sizeof(path), a.index, kind, sessionBootId(), "part");
  a.fh = LittleFS.open(path, "w");
  if (!a.fh) {
    mbedtls_sha256_free(&a.sha);
    g_st.writeErrors++;
    Serial.printf("[fs] could not open %s\n", path);
    return false;
  }
  a.open = true;

  FileEntry in;
  memset(&in, 0, sizeof(in));
  in.index = a.index;
  in.kind = kind;
  in.bytes = 0;
  in.bootId = sessionBootId();
  in.closed = false;
  in.mode = g_mode;
  in.hasSha = false;
  in.active = true;        // being written: retention never touches it
  in.hydrated = true;
  if (!trackEntry(in)) {
    // Unreachable with two writers and a cap of FS_MAX_FILES, but if it ever
    // happens the file must not exist unindexed: undo the open.
    a.fh.close();
    a.open = false;
    mbedtls_sha256_free(&a.sha);
    LittleFS.remove(path);
    g_st.writeErrors++;
    Serial.printf("[fs] index full with only active files; did not open #%lu\n",
                  (unsigned long)a.index);
    return false;
  }
  sortEntries();
  return true;
}

// Every byte that reaches the file goes through here, so the digest cannot
// drift from the contents -- there is no second path that writes.
static bool writeActive(Active &a, const uint8_t *data, size_t len) {
  if (!a.open || !len) return false;
  const size_t w = a.fh.write(data, len);
  if (w != len) {
    g_st.writeErrors++;
    return false;
  }
  mbedtls_sha256_update(&a.sha, data, len);
  a.bytes += (uint32_t)len;
  FileEntry *e = findEntry(a.index);
  if (e) e->bytes = a.bytes;
  return true;
}

// Commit buffered bytes on a time bound, so a crash costs a KNOWN amount of
// data rather than everything since the file was opened.
//
// 🐛 Before this existed, a Tier B .part with 12,612 bytes in it came back
// from a hard reset reporting 0 bytes. The name was on disk -- LittleFS
// commits that at create -- so the hub saw a crash artifact and correctly
// synced it as truncated, and it was truncated to NOTHING. Tier B is the
// month-over-month record; "a month that is gone cannot be re-measured" was
// already the stated reason it is deleted last, and it was being lost to
// something much more ordinary than a full disk.
//
// Deliberately NOT a flush per write: at the Tier A change-log rate that is
// thousands of syncs a minute and pointless flash wear. A time bound is also
// the thing that can be stated plainly in the docs -- at most
// FS_FLUSH_INTERVAL_MS of data, whatever the write rate.
static void flushDue(Active &a, uint32_t now) {
  if (!a.open) return;
  if ((uint32_t)(now - a.lastFlushMs) < FS_FLUSH_INTERVAL_MS) return;
  FS_SUB(FS_SUB_FLUSH);
  a.fh.flush();
  a.lastFlushMs = now;
}

static bool ensureOpen(Active &a, char kind) {
  if (a.open && a.bytes >= FS_FILE_MAX_BYTES) closeActive(a);
  if (!a.open) return openActive(a, kind);
  return true;
}

void filestoreNoteBusActivity() {
  g_lastBusMs.store(millis(), std::memory_order_relaxed);
  g_idleClosed.store(false, std::memory_order_relaxed);  // re-arm for the next key-off
}

bool filestoreIdleClosed() { return g_idleClosed.load(std::memory_order_relaxed); }

uint32_t filestoreBusQuietMs() {
  // No frame ever seen is NOT 'infinitely quiet'. Returning 0 means a
  // board that has never been on a bus can never satisfy the quiet test,
  // which is the safe reading: absence of traffic we never looked for is
  // not evidence the trip is over.
  return busQuietMs(millis(), g_lastBusMs.load(std::memory_order_relaxed));
}

void filestoreCloseActive() {
  closeActive(g_actSnapshot);
  closeActive(g_actChanges);
}

// ---------------------------------------------------------------------------
// Writers
// ---------------------------------------------------------------------------

static void writeSnapshotBlock() {
  FS_SUB(FS_SUB_SNAPSHOT);
  static SnifferRow rows[SNIFF_MAX_IDS];
  // sinceMs 0 = every known id, changed or not. A snapshot is a LATEST-VALUE
  // record: an id that stopped updating is a fact worth keeping, and omitting
  // it would make "this id went quiet" indistinguishable from "this id was
  // never there".
  const uint16_t n = snifferRowsChangedSince(rows, SNIFF_MAX_IDS, 0);
  if (!n) return;

  const bool fresh = !g_actSnapshot.open;
  if (!ensureOpen(g_actSnapshot, FS_KIND_SNAPSHOT)) return;

  if (fresh || g_actSnapshot.bytes == 0) {
    SnapFileHeader h = {};
    memcpy(h.magic, "CDGS", 4);
    h.version = 1;
    h.recLen = SNAP_REC_LEN;
    h.mode = g_mode;
    h.deviceId = sessionDeviceId();
    h.bootId = sessionBootId();
    writeActive(g_actSnapshot, (const uint8_t *)&h, sizeof(h));
  }

  uint8_t buf[6 + SNIFF_MAX_IDS * SNAP_REC_LEN];
  size_t p = 0;
  const uint32_t ms = millis();
  memcpy(buf + p, &ms, 4); p += 4;
  const uint16_t cnt = n;
  memcpy(buf + p, &cnt, 2); p += 2;
  for (uint16_t i = 0; i < n; i++) {
    // Bit 31 carries EXTENDED. A CAN id is 29 bits at most, so the top bit of
    // the u32 is free and the flag costs nothing. Spending a 14th byte on it
    // would be a 7.7% storage increase to carry one bit, and dropping it would
    // make an extended id indistinguishable from a standard one with the same
    // low bits -- which is a silent, unrecoverable corruption of the record.
    uint32_t id = rows[i].id & 0x1FFFFFFFu;
    if (rows[i].ext) id |= 0x80000000u;
    memcpy(buf + p, &id, 4); p += 4;
    buf[p++] = rows[i].dlc;
    memcpy(buf + p, rows[i].data, 8); p += 8;
  }
  writeActive(g_actSnapshot, buf, p);
}

static void drainChangeLog() {
  FS_SUB(FS_SUB_CHANGELOG);
  // recorderClear() (the 'k' key, and /api/rec) empties the log under us. The
  // cursor would then sit past the end forever and the drain would silently
  // stop, which looks exactly like "nothing is changing on the bus".
  if (g_chgCursor.row > recorderChangeStored()) {
    g_chgCursor.row = 0;
    g_chgCursor.headerDone = false;
  }
  if (!recorderRunning() && recorderChangeStored() == g_chgCursor.row) return;

  char chunk[FS_FLUSH_BUDGET_BYTES];
  RecCsvCursor before = g_chgCursor;
  const size_t n = recorderChangeCsvChunk(chunk, sizeof(chunk), &g_chgCursor);
  if (!n) return;

  if (!ensureOpen(g_actChanges, FS_KIND_CHANGES)) {
    g_chgCursor = before;      // put it back; nothing was written
    return;
  }
  // A rotation mid-stream starts a new file, which needs its own CSV header.
  if (g_actChanges.bytes == 0 && before.headerDone) {
    static const char hdr[] =
        "ms,id,ext,dlc,changed,d0,d1,d2,d3,d4,d5,d6,d7\n";
    writeActive(g_actChanges, (const uint8_t *)hdr, sizeof(hdr) - 1);
  }
  if (!writeActive(g_actChanges, (const uint8_t *)chunk, n)) {
    g_chgCursor = before;
    return;
  }

  // Reclaim the PSRAM the flushed rows are still occupying. Rare and bulk:
  // compaction memmoves the tail, so doing it every pass would be pure cost.
  const uint32_t cap = (uint32_t)recorderChangeCapacity();
  if (cap && g_chgCursor.row > (cap * FS_COMPACT_AT_PCT / 100)) {
    const uint32_t dropped = recorderChangeDiscardThrough(g_chgCursor.row);
    g_chgCursor.row -= dropped;
  }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// ⚠️ NVS FIRST, BEFORE THE MOUNT, and deliberately so.
//
// The loss record must be correct even on the paths where the filesystem is
// not: a failed mount, safe mode, a remote erase. Those are exactly when "has
// this device ever destroyed uncollected data?" matters most, and loading it
// afterwards would report a confident zero. NVS lives in its own partition,
// so a LittleFS format does not touch it: the counters survive the most
// destructive thing this device does, which is the point.
static void loadLossRecord() {
  memset(&g_st, 0, sizeof(g_st));
  g_st.ackedThrough = -1;
  g_fsPrefs.begin("cardiagfs", false);
  g_st.nextIndex    = g_fsPrefs.getULong("idx", 0);
  g_st.ackedThrough = (int32_t)g_fsPrefs.getLong("ack", -1);
  for (uint8_t s = 0; s < 3; s++) {
    char key[8];
    snprintf(key, sizeof(key), "lf%u", (unsigned)s);
    g_st.lostFiles[s] = g_fsPrefs.getULong(key, 0);
    snprintf(key, sizeof(key), "lb%u", (unsigned)s);
    g_st.lostBytes[s] = g_fsPrefs.getULong64(key, 0);
  }
  g_st.lostAckedFiles = g_fsPrefs.getULong("lostack", 0);
  if (const uint32_t lost = fsLostFilesTotal(&g_st)) {
    Serial.printf("[fs] LIFETIME LOSS RECORD: %lu file(s) of uncollected data "
                  "destroyed (A=%lu B=%lu C=%lu); hub has recorded %lu.%s\n",
                  (unsigned long)lost,
                  (unsigned long)g_st.lostFiles[0],
                  (unsigned long)g_st.lostFiles[1],
                  (unsigned long)g_st.lostFiles[2],
                  (unsigned long)g_st.lostAckedFiles,
                  lost > g_st.lostAckedFiles ? "  *** WARN STAYS SET ***" : "");
  }
  g_st.erases = g_fsPrefs.getULong("erases", 0);
}

void filestoreBeginSafeMode(uint8_t priorAttempts) {
  loadLossRecord();
  g_st.safeMode = true;
  g_st.bootAttempts = priorAttempts;
  g_st.mounted = false;
  Serial.printf("[fs] *** SAFE MODE: the last %u filestore start(s) never "
                "finished. NOT mounting, NOT formatting. Files on the partition "
                "are untouched. POST /api/v1/filestore/retry to try again, or "
                "/api/v1/filestore/erase to erase (both need X-Hub-Token). ***\n",
                (unsigned)priorAttempts);
}

bool filestoreSafeMode() { return g_st.safeMode; }

bool filestoreBegin() {
  loadLossRecord();
  g_st.bootAttempts = bootguardPriorAttempts();

  // Partition is labelled "spiffs" (see partitions_cardiag_8mb.csv), which is
  // what LittleFS.begin() looks for by default. Mounted at FS_VFS_ROOT
  // explicitly, because the scan and hydration use POSIX paths under it.
  //
  // ⚠️ formatOnFail=false, ALWAYS. This used to be true, which meant any mount
  // failure -- including a transient one -- silently destroyed every file the
  // hub had not collected, with no loss record because the index was never
  // built. A partition that will not mount now stays unmounted: the boot guard
  // counts the failed start, and after BG_FAIL_LIMIT of them the board comes
  // up in SAFE MODE, where erasing is a decision someone makes on purpose.
  const uint32_t t0 = millis();
  if (!LittleFS.begin(/*formatOnFail=*/false, FS_VFS_ROOT)) {
    g_st.mountMs = millis() - t0;
    Serial.println("[fs] LittleFS mount FAILED; NOT formatting. The logger "
                   "keeps running without persistence (rule 1); the boot guard "
                   "counts this as a failed start.");
    g_st.mounted = false;
    return false;
  }
  g_st.mountMs = millis() - t0;
  g_st.mounted = true;

  if (!LittleFS.exists(FS_DIR)) LittleFS.mkdir(FS_DIR);
  const uint32_t t1 = millis();
  scanDir();
  g_st.scanMs = millis() - t1;

  Serial.printf("[fs] mounted %lu/%lu B, %u files (%u open), next index %lu, "
                "acked through %ld\n",
                (unsigned long)LittleFS.usedBytes(),
                (unsigned long)LittleFS.totalBytes(),
                g_tab.count, g_st.openFiles,
                (unsigned long)g_st.nextIndex, (long)g_st.ackedThrough);

  // ⭐ THE NUMBERS FS_BOOT_WDT_S IS DERIVED FROM. Printed every boot so the
  // budget is set from a measurement, and so a scan creeping towards its
  // budget is seen in serial.log long before it becomes a watchdog reset.
  const uint32_t budgetMs = (uint32_t)FS_BOOT_WDT_S * 1000u;
  const uint32_t usedMs = g_st.mountMs + g_st.scanMs;
  Serial.printf("[fs] BOOT TIMING: mount %lu ms + scan %lu ms (%lu entries, "
                "%lu files) = %lu ms of %lu ms budget (%lu%%)%s\n",
                (unsigned long)g_st.mountMs, (unsigned long)g_st.scanMs,
                (unsigned long)g_st.scanEntries, (unsigned long)g_st.scanFiles,
                (unsigned long)usedMs, (unsigned long)budgetMs,
                (unsigned long)(usedMs * 100u / budgetMs),
                usedMs * 3u > budgetMs ? "  *** OVER 1/3 OF BUDGET ***" : "");
  if (g_tab.count > FS_MAX_FILES) {
    Serial.printf("[fs] %u files on disk, cap %u: retention will evict %u "
                  "oldest-first over the next few seconds, each one logged\n",
                  g_tab.count, (unsigned)FS_MAX_FILES,
                  (unsigned)(g_tab.count - FS_MAX_FILES));
  }

  // ⭐ THE BOARD STATES ITS OWN EFFECTIVE LIMITS, so no run has to infer them
  // from the env it believes it flashed. A `-D` flag that a header quietly
  // redefines is invisible in every artifact a bench run produces -- it cost
  // claim 2 a fourth wasted run on 2026-09-24. serial.log is captured for the
  // whole window, so this line makes that class of failure legible.
  Serial.printf("[fs] EFFECTIVE CAPS: tier A max %u%% = %lu B, warn at %u%%, "
                "snapshot %lu ms\n",
                (unsigned)FS_TIER_A_MAX_PCT,
                (unsigned long)((uint64_t)usableBytes() * FS_TIER_A_MAX_PCT / 100),
                (unsigned)FS_WARN_USAGE_PCT,
                (unsigned long)FS_SNAPSHOT_PERIOD_MS);

  if (g_st.openFiles) {
    Serial.printf("[fs] %u file(s) left open by an earlier boot -- crash "
                  "artifacts. They are listed closed=false and the hub syncs "
                  "them as truncated rather than waiting forever.\n",
                  g_st.openFiles);
  }
  g_nextSnapMs = millis();
  return true;
}

void filestoreSetEnabled(bool on) {
  if (on == g_enabled) return;
  g_enabled = on;
  if (!on) filestoreCloseActive();
  else g_nextSnapMs = millis();
}

bool filestoreEnabled() { return g_enabled; }

void filestoreSetMode(uint8_t mode) {
  if (mode == g_mode) return;
  // Close first: a file must not contain rows from two modes, or a synthetic
  // bench run and a real capture end up in one record with one label.
  filestoreCloseActive();
  g_mode = mode;
}

static void filestoreTick();

void filestoreLoop() {
  if (!g_st.mounted || !g_enabled) return;
  const uint32_t t0 = micros();
  fsProfBeginPass(&g_prof);
  filestoreTick();
  uint32_t worstUs = 0;
  const uint8_t worst = fsProfEndPass(&g_prof, &worstUs);
  const uint32_t passUs = micros() - t0;
  fsSubWindowNote(&g_subWin[0], worst, worstUs, passUs);
  fsSubWindowNote(&g_subWin[1], worst, worstUs, passUs);
  fsSubWindowNote(&g_subBoot, worst, worstUs, passUs);
}

FsSubWindow filestoreTakeSubWindow(uint8_t which) {
  const FsSubWindow w = g_subWin[which & 1];
  g_subWin[which & 1] = FsSubWindow{};
  return w;
}

FsSubWindow filestoreSubBoot() { return g_subBoot; }

static void filestoreTick() {
  const uint32_t now = millis();
  if ((int32_t)(now - g_nextSnapMs) >= 0) {
    g_nextSnapMs = now + FS_SNAPSHOT_PERIOD_MS;
    writeSnapshotBlock();
  }
  drainChangeLog();

  // ⭐ CLEAN KEY-OFF. The bus going quiet is the only warning there is that
  // INH is about to drop and take the 3.3 V rail with it, so everything gets
  // closed properly NOW -- digest, .meta, rename -- while there is still
  // power. After this, losing the rail costs literally nothing.
  //
  // Deliberately not reliant on flushDue(): the 10 s bound is the crash
  // backstop, and on a normal key-off there must be nothing left to lose
  // rather than up to ten seconds of it.
  //
  // `now` was sampled at the top of this pass; canTask may have stored a newer
  // timestamp since. One read, signed comparison (busidle.h).
  const uint32_t lastBus = g_lastBusMs.load(std::memory_order_relaxed);
  if (!g_idleClosed.load(std::memory_order_relaxed) &&
      busIdleFor(now, lastBus, CAN_BUS_IDLE_CLOSE_MS) &&
      (g_actSnapshot.open || g_actChanges.open)) {
    const uint32_t a = g_actSnapshot.bytes, b = g_actChanges.bytes;
    filestoreCloseActive();
    g_idleClosed.store(true, std::memory_order_relaxed);
    Serial.printf("[fs] bus idle %lums -> closed all files "
                  "(tierB=%lu B, tierA=%lu B); safe to lose power\n",
                  (unsigned long)busQuietMs(now, lastBus),
                  (unsigned long)a, (unsigned long)b);
  }

  // Bound what a power cut or a panic can cost. Both tiers, every pass.
  flushDue(g_actSnapshot, now);
  flushDue(g_actChanges, now);

  g_st.rowsDropped = recorderChangeDropped();

  // Fill in sizes/digests the boot scan skipped, a few per pass.
  hydrateSome(FS_HYDRATE_PER_PASS);

  // Every 5 s normally; every pass while a backlog is draining.
  static uint32_t lastCheck = 0;
  if (g_retentionMore || now - lastCheck >= 5000) {
    lastCheck = now;
    enforceRetention();
  }
}

const FileStoreStats *filestoreStats() { return &g_st; }

uint8_t filestoreUsagePct() {
  const uint32_t total = LittleFS.totalBytes();
  if (!total) return 0;
  return (uint8_t)((uint64_t)LittleFS.usedBytes() * 100 / total);
}

bool filestoreWarn() {
  // Arm 1: the partition is filling. Arm 2: data has been destroyed that the
  // hub has not yet written down.
  //
  // ⭐ Arm 2 compares two NVS-backed lifetime numbers, NOT a RAM flag. An
  // earlier version latched on `deletedUnacked > 0`, which was erased by the
  // key-off that ends every trip -- the warning died with the counter and the
  // next ignition reported all clear while the data was still missing.
  //
  // It clears only when the hub says it has recorded the loss, which is the
  // right condition: the point was never "somebody saw a flag", it was "the
  // loss is written down somewhere that survives this device".
  return filestoreUsagePct() >= FS_WARN_USAGE_PCT ||
         fsLostFilesTotal(&g_st) > g_st.lostAckedFiles;
}

uint32_t filestoreAckLoss(uint32_t throughFiles) {
  // A watermark, like the file ack: idempotent and monotonic. Clamped to the
  // real total so a hub cannot acknowledge losses that have not happened, and
  // never moved backwards -- a hub that lost its own state must not be able to
  // silence a warning for a loss it never recorded.
  const uint32_t total = fsLostFilesTotal(&g_st);
  if (throughFiles > total) throughFiles = total;
  if (throughFiles > g_st.lostAckedFiles) {
    g_st.lostAckedFiles = throughFiles;
    g_fsPrefs.putULong("lostack", throughFiles);
    Serial.printf("[fs] hub recorded the loss of %lu file(s) of %lu; warn %s\n",
                  (unsigned long)throughFiles, (unsigned long)total,
                  filestoreWarn() ? "STAYS SET" : "clears");
  }
  return g_st.lostAckedFiles;
}

int32_t filestoreAck(int32_t throughIndex) {
  // Protocol 2.1: a watermark, idempotent, and it never moves backwards. A
  // retransmitted ack is normal; a LOWER one means the hub lost state, and
  // honouring it would re-send data that is already stored.
  if (throughIndex > g_st.ackedThrough) {
    g_st.ackedThrough = throughIndex;
    g_fsPrefs.putLong("ack", throughIndex);
  }
  return g_st.ackedThrough;
}

// ---------------------------------------------------------------------------
// HTTP (protocol 2)
// ---------------------------------------------------------------------------

static bool fsTokenOk(WebServer &srv) {
  if (!srv.hasHeader("X-Hub-Token")) return false;
  const String got = srv.header("X-Hub-Token");
  const char *want = HUB_API_TOKEN;
  const size_t wlen = strlen(want);
  uint8_t diff = (uint8_t)(got.length() ^ wlen);
  for (size_t i = 0; i < wlen; i++) {
    const char c = (i < got.length()) ? got[i] : 0;
    diff |= (uint8_t)(c ^ want[i]);
  }
  return diff == 0;
}

static void handleList(WebServer &srv) {
  // Streamed, not built in RAM: FS_MAX_FILES entries of ~170 chars is 16 KB,
  // and right after boot on an over-full disk the index can be many times
  // that. A heap spike this board should not take for a listing.
  srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
  srv.send(200, "application/json", "");

  char buf[256];
  srv.sendContent("[");
  for (uint16_t i = 0; i < g_tab.count; i++) {
    // Every entry is listed, hydrated or not yet -- on demand here, so the
    // hub never sees a placeholder size or a missing digest on a closed file.
    // Each read is one path lookup; fed per entry, bought with progress.
    if (!g_tab.v[i].hydrated) {
      hydrate(&g_tab.v[i]);
      wdtFeedIfArmed();
    }
    const FileEntry &e = g_tab.v[i];
    // "tier" is the RETENTION class and "kind" is what is in the file. They
    // are both published because they answer different questions: the hub
    // sorts retention by tier and picks a parser by kind, and Tier A carries
    // two kinds, so neither can be inferred from the other.
    const char tier = fsTierForKind(e.kind);
    int n = snprintf(buf, sizeof(buf),
        "%s{\"index\":%lu,\"name\":\"%06lu_%c%c_%08lX.%s\",\"bytes\":%lu,"
        "\"boot_id\":%lu,\"tier\":\"%c\",\"kind\":\"%s\",\"closed\":%s,"
        "\"synthetic\":%s,\"format\":\"%s\",\"sha256\":",
        i ? "," : "",
        (unsigned long)e.index, (unsigned long)e.index, tier, e.kind,
        (unsigned long)e.bootId, e.closed ? "log" : "part",
        (unsigned long)e.bytes, (unsigned long)e.bootId, tier,
        fsKindName(e.kind),
        e.closed ? "true" : "false",
        // 🔑 THREE-VALUED, AND null IS NOT false. A file whose producing mode
        // could not be recovered is UNKNOWN provenance, and rendering that as
        // "synthetic":false would be a claim that it came from the car. The
        // hub's policy for null is its own (do not pool into a baseline); the
        // logger's job is only to not lie about what it knows.
        (e.mode == FS_MODE_UNKNOWN) ? "null"
                                    : ((e.mode == MODE_SELFTEST) ? "true"
                                                                 : "false"),
        (e.kind == FS_KIND_SNAPSHOT) ? "cdgs1" : "csv");
    srv.sendContent(buf, n);

    if (e.hasSha) {
      char hex[70];
      hex[0] = '"';
      for (int b = 0; b < 32; b++) snprintf(hex + 1 + b * 2, 3, "%02x", e.sha[b]);
      hex[65] = '"';
      hex[66] = 0;
      srv.sendContent(hex, 66);
    } else {
      // Never a placeholder digest. An open file has no digest, and inventing
      // one would make the hub's integrity check pass on a truncated file.
      srv.sendContent("null", 4);
    }
    srv.sendContent("}", 1);
  }
  srv.sendContent("]");
  srv.sendContent("");
}

// Protocol 2: GET /api/v1/files/<index>, with Range so an interrupted sync
// resumes instead of restarting. On a link that drops every couple of minutes
// -- which is the whole reason hublink exists -- restarting a 64 KB transfer
// each time can mean never finishing one.
static void handleFetch(WebServer &srv) {
  const uint32_t index = (uint32_t)strtoul(srv.pathArg(0).c_str(), nullptr, 10);
  const FileEntry *e = findEntry(index);
  if (!e) {
    srv.send(404, "application/json", "{\"error\":\"no such index\"}");
    return;
  }

  char path[64];
  makeName(path, sizeof(path), e->index, e->kind, e->bootId,
           e->closed ? "log" : "part");
  File f = LittleFS.open(path, "r");
  if (!f) {
    srv.send(500, "application/json", "{\"error\":\"open failed\"}");
    return;
  }

  const size_t total = f.size();
  size_t start = 0, end = total ? total - 1 : 0;
  bool partial = false;

  if (srv.hasHeader("Range")) {
    const String r = srv.header("Range");
    if (r.startsWith("bytes=")) {
      const int dash = r.indexOf('-');
      if (dash > 6) {
        start = (size_t)strtoul(r.substring(6, dash).c_str(), nullptr, 10);
        const String tail = r.substring(dash + 1);
        if (tail.length()) {
          const size_t e2 = (size_t)strtoul(tail.c_str(), nullptr, 10);
          if (e2 < end) end = e2;
        }
        partial = true;
      }
    }
    if (partial && start >= total) {
      f.close();
      char hdr[64];
      snprintf(hdr, sizeof(hdr), "bytes */%u", (unsigned)total);
      srv.sendHeader("Content-Range", hdr);
      srv.send(416, "application/json",
               "{\"error\":\"range beyond end of file\"}");
      return;
    }
  }

  const size_t len = (total == 0) ? 0 : (end - start + 1);
  const char *ctype = (e->kind == FS_KIND_SNAPSHOT)
                          ? "application/octet-stream" : "text/csv";

  srv.sendHeader("Accept-Ranges", "bytes");
  if (partial) {
    char hdr[80];
    snprintf(hdr, sizeof(hdr), "bytes %u-%u/%u",
             (unsigned)start, (unsigned)end, (unsigned)total);
    srv.sendHeader("Content-Range", hdr);
  }
  srv.setContentLength(len);
  srv.send(partial ? 206 : 200, ctype, "");

  f.seek(start);
  uint8_t buf[REC_CSV_CHUNK];
  size_t left = len;
  while (left) {
    const size_t want = left < sizeof(buf) ? left : sizeof(buf);
    const size_t got = f.read(buf, want);
    if (!got) break;
    srv.sendContent((const char *)buf, got);
    left -= got;
    // ⭐ FEED THE WATCHDOG, BUT ONLY HAVING MADE PROGRESS.
    //
    // This loop runs inside loop()'s WDT window and sendContent() blocks on
    // the socket. The biggest file the partition can hold, over a marginal
    // link, can exceed WDT_TIMEOUT_S -- and a watchdog that fires during a
    // legitimate download reboots the board mid-transfer, forever, which is
    // strictly worse than no watchdog.
    //
    // The feed is AFTER `got` bytes were actually sent, never at the top of
    // the loop: it is bought with progress. A stall inside a single
    // sendContent() still trips the watchdog, which is the case it is for.
    wdtFeedIfArmed();
  }
  f.close();
}

static void handleAck(WebServer &srv) {
  if (!fsTokenOk(srv)) {
    srv.send(401, "application/json",
             "{\"error\":\"bad or missing X-Hub-Token\"}");
    return;
  }
  const String body = srv.arg("plain");
  const int k = body.indexOf("\"through_index\"");
  if (k < 0) {
    srv.send(400, "application/json", "{\"error\":\"need through_index\"}");
    return;
  }
  const int c = body.indexOf(':', k);
  const long through = (c >= 0) ? atol(body.c_str() + c + 1) : -1;
  const int32_t now = filestoreAck((int32_t)through);

  // OPTIONAL second watermark, over the LIFETIME loss record rather than over
  // files: "I have durably stored the loss record up to N lost files."
  //
  // It rides on this endpoint because the hub already calls it every sync and
  // it is already authenticated -- a separate endpoint would be a second round
  // trip and a second thing to get wrong. Omitting the field is not an
  // acknowledgement: a hub that does not know about loss records leaves the
  // warning exactly where it is, which is the safe default.
  uint32_t lossAcked = g_st.lostAckedFiles;
  const int lk = body.indexOf("\"loss_recorded_files\"");
  if (lk >= 0) {
    const int lc = body.indexOf(':', lk);
    if (lc >= 0) lossAcked = filestoreAckLoss(
        (uint32_t)strtoul(body.c_str() + lc + 1, nullptr, 10));
  }

  enforceRetention();

  char buf[240];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"acked_through\":%ld,\"requested\":%ld,"
           "\"files\":%u,\"usage_pct\":%u,"
           "\"lost_files_total\":%lu,\"loss_recorded_files\":%lu,\"warn\":%s}",
           (long)now, through, g_tab.count, filestoreUsagePct(),
           (unsigned long)fsLostFilesTotal(&g_st), (unsigned long)lossAcked,
           filestoreWarn() ? "true" : "false");
  srv.send(200, "application/json", buf);
}

// ---------------------------------------------------------------------------
// Safe mode (bootguard.h)
// ---------------------------------------------------------------------------

static bool safeModeGate(WebServer &srv) {
  if (!fsTokenOk(srv)) {
    srv.send(401, "application/json",
             "{\"error\":\"bad or missing X-Hub-Token\"}");
    return false;
  }
  if (!g_st.safeMode) {
    srv.send(409, "application/json",
             "{\"error\":\"not in safe mode; the filestore is running\"}");
    return false;
  }
  return true;
}

// POST /api/v1/filestore/retry -- clear the boot guard and reboot into a
// normal filestore start. For when the failure was transient, or a fix has
// been flashed... though a new build resets the guard by itself (appTag).
static void handleRetry(WebServer &srv) {
  if (!safeModeGate(srv)) return;
  bootguardClear();
  srv.send(200, "application/json",
           "{\"ok\":true,\"action\":\"retry\",\"rebooting\":true}");
  Serial.println("[fs] safe mode: retry requested; rebooting");
  delay(200);                // let the response leave
  ESP.restart();
}

// POST /api/v1/filestore/erase  body {"confirm":"erase"}
//
// ⚠️ THE ONLY WAY THIS FIRMWARE FORMATS THE PARTITION. Never automatic: it
// destroys every file the hub has not collected, and in safe mode the index
// was never built, so those files CANNOT be counted into the loss record. The
// lifetime `erases` counter is bumped instead, so the event itself survives.
static void handleErase(WebServer &srv) {
  if (!safeModeGate(srv)) return;
  const String body = srv.arg("plain");
  if (body.indexOf("\"confirm\"") < 0 || body.indexOf("\"erase\"") < 0) {
    srv.send(400, "application/json",
             "{\"error\":\"need {\\\"confirm\\\":\\\"erase\\\"}\"}");
    return;
  }
  g_st.erases++;
  g_fsPrefs.putULong("erases", g_st.erases);
  Serial.printf("[fs] *** REMOTE ERASE requested (lifetime erase #%lu). Files "
                "on the partition were never indexed and are NOT counted in "
                "the loss record. ***\n", (unsigned long)g_st.erases);
  // A format outlasts any sane watchdog window, and a watchdog that fired
  // mid-format would reboot into safe mode with a half-formatted partition.
  // Unsubscribe for the duration; the restart below ends it either way.
  esp_task_wdt_delete(nullptr);
  const bool ok = LittleFS.format();
  bootguardClear();
  char buf[96];
  snprintf(buf, sizeof(buf),
           "{\"ok\":%s,\"action\":\"erase\",\"erases\":%lu,\"rebooting\":true}",
           ok ? "true" : "false", (unsigned long)g_st.erases);
  srv.send(ok ? 200 : 500, "application/json", buf);
  Serial.printf("[fs] erase %s; rebooting\n", ok ? "done" : "FAILED");
  delay(200);
  ESP.restart();
}

void filestoreRegister(WebServer &srv) {
  srv.on("/api/v1/filestore/retry", HTTP_POST, [&srv]() { handleRetry(srv); });
  srv.on("/api/v1/filestore/erase", HTTP_POST, [&srv]() { handleErase(srv); });
  srv.on("/api/v1/files", HTTP_GET, [&srv]() { handleList(srv); });
  srv.on("/api/v1/files/ack", HTTP_POST, [&srv]() { handleAck(srv); });
  // Registered AFTER /ack so the literal route wins; UriBraces would otherwise
  // match "ack" as an index and quietly 404 every acknowledgement.
  srv.on(UriBraces("/api/v1/files/{}"), HTTP_GET, [&srv]() { handleFetch(srv); });
}

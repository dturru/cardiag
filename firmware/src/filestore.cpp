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

#include "filestore.h"
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

struct FileEntry {
  uint32_t index;
  uint32_t bytes;
  uint32_t bootId;
  uint8_t  sha[32];
  char     kind;          // tier is fsTierForKind(kind); never stored twice
  uint8_t  mode;
  bool     closed;
  bool     hasSha;
};

static FileEntry g_files[FS_MAX_FILES];
static uint16_t  g_fileCount = 0;
static FileStoreStats g_st;

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

// Bus-idle tracking for the clean key-off close. Written by the CAN task,
// read by loop(); a 32-bit aligned store is atomic on this core, and being a
// few milliseconds stale cannot matter against a 3 s threshold.
static volatile uint32_t g_lastBusMs = 0;
static bool g_idleClosed = false;
static RecCsvCursor g_chgCursor = {0, false};

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

static void makeName(char *out, size_t cap, uint32_t index, char kind,
                     uint32_t bootId, const char *ext) {
  snprintf(out, cap, FS_DIR "/%06lu_%c%c_%08lX.%s",
           (unsigned long)index, fsTierForKind(kind), kind,
           (unsigned long)bootId, ext);
}

static bool parseName(const char *name, uint32_t *index, char *kind,
                      uint32_t *bootId, char *ext, size_t extCap) {
  // NNNNNN_TK_BBBBBBBB.ext
  unsigned long idx = 0, boot = 0;
  char t = 0, k = 0;
  char e[8] = {0};
  if (sscanf(name, "%6lu_%c%c_%8lX.%7s", &idx, &t, &k, &boot, e) != 5) return false;
  if (k != FS_KIND_RAW && k != FS_KIND_CHANGES &&
      k != FS_KIND_SNAPSHOT && k != FS_KIND_BOOKEND) return false;
  // The tier is DERIVED from the kind, never read from the name: a file whose
  // two letters disagree is a file written by a version that had the mapping
  // wrong, and trusting its tier letter would put it in the wrong retention
  // class. The kind is the fact; the tier is a function of it.
  if (t != fsTierForKind(k)) {
    Serial.printf("[fs] %s: tier '%c' disagrees with kind '%c'; using '%c'\n",
                  name, t, k, fsTierForKind(k));
  }
  *index = (uint32_t)idx;
  *kind = k;
  *bootId = (uint32_t)boot;
  snprintf(ext, extCap, "%s", e);
  return true;
}

// ---------------------------------------------------------------------------
// Index table
// ---------------------------------------------------------------------------

static FileEntry *findEntry(uint32_t index) {
  for (uint16_t i = 0; i < g_fileCount; i++) {
    if (g_files[i].index == index) return &g_files[i];
  }
  return nullptr;
}

static FileEntry *addEntry(uint32_t index) {
  FileEntry *e = findEntry(index);
  if (e) return e;
  if (g_fileCount >= FS_MAX_FILES) return nullptr;
  e = &g_files[g_fileCount++];
  memset(e, 0, sizeof(*e));
  e->index = index;
  return e;
}

static void sortEntries() {
  // Insertion sort: at most FS_MAX_FILES entries and nearly sorted already.
  for (uint16_t i = 1; i < g_fileCount; i++) {
    FileEntry tmp = g_files[i];
    int16_t j = (int16_t)i - 1;
    while (j >= 0 && g_files[j].index > tmp.index) {
      g_files[j + 1] = g_files[j];
      j--;
    }
    g_files[j + 1] = tmp;
  }
}

static void recomputeUsage() {
  g_st.files = g_fileCount;
  g_st.openFiles = 0;
  g_st.pendingUnacked = 0;
  g_st.tierABytes = g_st.tierBBytes = g_st.tierCBytes = 0;
  for (uint16_t i = 0; i < g_fileCount; i++) {
    const FileEntry &e = g_files[i];
    if (!e.closed) g_st.openFiles++;
    // Closed and above the watermark: finished, and the hub does not have it.
    // Only the logger can count this -- the hub cannot derive it from files,
    // open and acked_through, because evictions punch holes in the index
    // range. Between trips this is normally non-zero; see protocol 2.3.1.
    else if ((int32_t)e.index > g_st.ackedThrough) g_st.pendingUnacked++;
    switch (fsTierForKind(e.kind)) {
      case FS_TIER_SNAPSHOT: g_st.tierBBytes += e.bytes; break;
      case FS_TIER_BOOKEND:  g_st.tierCBytes += e.bytes; break;
      default:               g_st.tierABytes += e.bytes; break;
    }
  }
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
static uint8_t readModeFromContent(uint32_t index, char kind, uint32_t bootId) {
  // Only Tier B is self-describing. A CSV has a column header and no room for
  // provenance, so for those kinds the mode is genuinely unrecoverable and
  // must stay UNKNOWN rather than being guessed at.
  if (kind != FS_KIND_SNAPSHOT) return FS_MODE_UNKNOWN;

  char path[64];
  makeName(path, sizeof(path), index, kind, bootId, "part");
  File f = LittleFS.open(path, "r");
  if (!f) return FS_MODE_UNKNOWN;
  SnapFileHeader h{};
  const size_t n = f.read((uint8_t *)&h, sizeof(h));
  f.close();
  if (n != sizeof(h) || memcmp(h.magic, "CDGS", 4) != 0) return FS_MODE_UNKNOWN;
  return h.mode;
}

static void readMeta(FileEntry *e, uint32_t bootId) {
  char path[64];
  makeName(path, sizeof(path), e->index, e->kind, bootId, "meta");
  File f = LittleFS.open(path, "r");
  if (!f) return;
  char line[128];
  while (f.available()) {
    const size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    if (strncmp(line, "sha256=", 7) == 0 && strlen(line + 7) >= 64) {
      for (int i = 0; i < 32; i++) {
        char b[3] = {line[7 + i * 2], line[8 + i * 2], 0};
        e->sha[i] = (uint8_t)strtoul(b, nullptr, 16);
      }
      e->hasSha = true;
    } else if (strncmp(line, "mode=", 5) == 0) {
      e->mode = (uint8_t)atoi(line + 5);
    }
  }
  f.close();
}

static void scanDir() {
  g_fileCount = 0;
  File dir = LittleFS.open(FS_DIR);
  if (!dir || !dir.isDirectory()) {
    LittleFS.mkdir(FS_DIR);
    return;
  }
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const char *full = f.name();
    // LittleFS may hand back either a bare name or a full path depending on
    // the core version. Take the last segment either way rather than assuming.
    const char *base = strrchr(full, '/');
    base = base ? base + 1 : full;

    uint32_t index = 0, bootId = 0;
    char kind = 0, ext[8] = {0};
    if (!parseName(base, &index, &kind, &bootId, ext, sizeof(ext))) continue;
    if (strcmp(ext, "meta") == 0) continue;     // read via its .log below

    FileEntry *e = addEntry(index);
    if (!e) continue;
    e->kind = kind;
    e->bootId = bootId;
    e->bytes = (uint32_t)f.size();
    e->closed = (strcmp(ext, "log") == 0);
    e->mode = FS_MODE_UNKNOWN;
    if (e->closed) readMeta(e, bootId);
    else e->mode = readModeFromContent(index, kind, bootId);
  }
  dir.close();
  sortEntries();
  recomputeUsage();
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

static void dropEntry(uint16_t i) {
  for (uint16_t j = i; j + 1 < g_fileCount; j++) g_files[j] = g_files[j + 1];
  g_fileCount--;
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
  g_st.unackedEvictedBytes[slot] += e.bytes;
  g_st.unackedEvictedFiles[slot]++;
  // Loud on purpose. This line is the only trace a field unit leaves of data
  // the hub will now never receive.
  Serial.printf("[fs] *** EVICTED UNACKED TIER %c (%s) #%lu, %lu B -- DATA LOST "
                "(tier %c total now %u files / %lu B) ***\n",
                tier, fsKindName(e.kind), (unsigned long)e.index,
                (unsigned long)e.bytes, tier,
                (unsigned)g_st.unackedEvictedFiles[slot],
                (unsigned long)g_st.unackedEvictedBytes[slot]);
}

// Deletes exactly one file, choosing by the protocol's order. Returns false
// when there is nothing left that may be deleted.
static bool evictOne() {
  const int32_t ack = g_st.ackedThrough;

  // 1. Acked files, oldest first. The hub has them; they cost nothing to lose.
  for (uint16_t i = 0; i < g_fileCount; i++) {
    if (!g_files[i].closed) continue;
    if ((int32_t)g_files[i].index > ack) continue;
    Serial.printf("[fs] evict acked #%lu\n", (unsigned long)g_files[i].index);
    removeFiles(g_files[i]);
    dropEntry(i);
    g_st.deletedAcked++;
    return true;
  }

  // 2. Unacked Tier A (raw frames / change log), oldest first. Sub-second
  //    detail: painful to lose, and collectable again on the next drive.
  // 3. Unacked Tier B. The month-over-month record; a month that is gone
  //    cannot be re-measured. Reaching here is a reportable event.
  // 4. Unacked Tier C LAST, and mostly a formality -- trip bookends are bytes
  //    per trip, so deleting one frees nothing. If C is what stands between
  //    this device and a full disk, the disk is not the problem.
  for (char tier : {FS_TIER_RAW, FS_TIER_SNAPSHOT, FS_TIER_BOOKEND}) {
    for (uint16_t i = 0; i < g_fileCount; i++) {
      if (!g_files[i].closed) continue;
      if (fsTierForKind(g_files[i].kind) != tier) continue;
      removeFiles(g_files[i]);
      noteUnackedEviction(g_files[i]);
      dropEntry(i);
      return true;
    }
  }
  return false;   // only open files remain; never delete what is being written
}

static uint32_t usableBytes() {
  const uint32_t total = LittleFS.totalBytes();
  return total ? total : (uint32_t)HUB_FS_USABLE_BYTES;
}

// Finds the oldest closed Tier A file, preferring acked ones. Returns
// g_fileCount when there is no candidate.
//
// ⚠️ The two passes are the point. g_files is sorted by index, so a single
// oldest-first sweep picks up whatever comes first -- which, once the hub has
// acked a prefix and the newer files are still unacked, is an ACKED file by
// luck of ordering rather than by rule. The moment an ack arrives out of that
// shape the same sweep destroys unacked data while acked copies the hub
// already holds sit right next to it.
static uint16_t oldestTierACandidate(bool wantAcked) {
  for (uint16_t i = 0; i < g_fileCount; i++) {
    if (!g_files[i].closed) continue;
    if (fsTierForKind(g_files[i].kind) != FS_TIER_RAW) continue;
    const bool acked = (int32_t)g_files[i].index <= g_st.ackedThrough;
    if (acked == wantAcked) return i;
  }
  return g_fileCount;
}

// Keeps Tier A inside its share. Without this, Tier A's 50:1 rate advantage
// lets it fill the partition between two snapshot blocks.
//
// Deletion order inside the cap mirrors the global one: acked first, because
// the hub already has those and they cost nothing to lose.
static void enforceTierACap() {
  const uint32_t cap = (uint32_t)((uint64_t)usableBytes() * FS_TIER_A_MAX_PCT / 100);
  while (g_st.tierABytes > cap) {
    bool acked = true;
    uint16_t i = oldestTierACandidate(true);
    if (i == g_fileCount) {
      acked = false;
      i = oldestTierACandidate(false);
    }
    if (i == g_fileCount) break;      // only open Tier A left; leave it alone

    removeFiles(g_files[i]);
    if (acked) g_st.deletedAcked++;
    else       noteUnackedEviction(g_files[i]);
    dropEntry(i);
    recomputeUsage();
    // Bought with progress: one file is provably gone. A full partition can
    // put many removes plus a recomputeUsage() back to back, and this runs
    // inside loop()'s WDT window. The loop terminates on its own -- tierABytes
    // strictly decreases, and it breaks when no candidate remains.
    esp_task_wdt_reset();
  }
}

static void enforceRetention() {
  recomputeUsage();
  enforceTierACap();
  // LittleFS needs slack to do anything at all, including delete. Reclaim
  // before it is full rather than at the moment a write fails.
  const uint32_t limit = (uint32_t)((uint64_t)usableBytes() * 90 / 100);
  uint8_t guard = 0;
  while (LittleFS.usedBytes() > limit && guard++ < FS_MAX_FILES) {
    if (!evictOne()) break;
    recomputeUsage();
    // Same contract as the cap: fed only after a file was provably deleted,
    // and `guard` bounds the loop at FS_MAX_FILES regardless. A full-disk
    // reclaim is a legitimate long operation; rebooting through it would drop
    // the open files it is trying to make room for.
    esp_task_wdt_reset();
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

static void closeActive(Active &a) {
  if (!a.open) return;
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

  FileEntry *e = addEntry(a.index);
  if (e) {
    e->kind = a.kind;
    e->bytes = a.bytes;
    e->bootId = boot;
    e->closed = true;
    e->mode = g_mode;
    e->hasSha = true;
    memcpy(e->sha, digest, 32);
  }
  sortEntries();
  recomputeUsage();
  Serial.printf("[fs] closed #%lu tier %c kind %s, %lu B\n",
                (unsigned long)a.index, fsTierForKind(a.kind),
                fsKindName(a.kind), (unsigned long)a.bytes);
}

static bool openActive(Active &a, char kind) {
  enforceRetention();

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

  FileEntry *e = addEntry(a.index);
  if (e) {
    e->kind = kind;
    e->bytes = 0;
    e->bootId = sessionBootId();
    e->closed = false;
    e->mode = g_mode;
    e->hasSha = false;
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
  a.fh.flush();
  a.lastFlushMs = now;
}

static bool ensureOpen(Active &a, char kind) {
  if (a.open && a.bytes >= FS_FILE_MAX_BYTES) closeActive(a);
  if (!a.open) return openActive(a, kind);
  return true;
}

void filestoreNoteBusActivity() {
  g_lastBusMs = millis();
  g_idleClosed = false;     // traffic is back; re-arm for the next key-off
}

bool filestoreIdleClosed() { return g_idleClosed; }

uint32_t filestoreBusQuietMs() {
  // No frame ever seen is NOT 'infinitely quiet'. Returning 0 means a
  // board that has never been on a bus can never satisfy the quiet test,
  // which is the safe reading: absence of traffic we never looked for is
  // not evidence the trip is over.
  if (!g_lastBusMs) return 0;
  return (uint32_t)(millis() - g_lastBusMs);
}

void filestoreCloseActive() {
  closeActive(g_actSnapshot);
  closeActive(g_actChanges);
}

// ---------------------------------------------------------------------------
// Writers
// ---------------------------------------------------------------------------

static void writeSnapshotBlock() {
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

bool filestoreBegin() {
  memset(&g_st, 0, sizeof(g_st));
  g_st.ackedThrough = -1;

  // Partition is labelled "spiffs" (see partitions_cardiag_8mb.csv), which is
  // what LittleFS.begin() looks for by default.
  //
  // ⭐ THE FORMAT IS SAFE FROM THE WATCHDOG BY CONSTRUCTION, NOT BY LUCK.
  // formatOnFail=true can take far longer than WDT_TIMEOUT_S on a corrupt
  // partition, but filestoreBegin() is called from setup() and the task
  // watchdog is not armed until AFTER setup() has got this far -- so there is
  // no window in which a format can trip it. ⚠️ Moving the arming earlier, or
  // moving a format into loop(), reintroduces the hazard: a watchdog firing
  // mid-format would reboot into the same format, forever.
  if (!LittleFS.begin(/*formatOnFail=*/true)) {
    Serial.println("[fs] LittleFS mount FAILED; the logger keeps running "
                   "without persistence (rule 1) but files are not truth "
                   "until this is fixed.");
    g_st.mounted = false;
    return false;
  }
  g_st.mounted = true;

  g_fsPrefs.begin("cardiagfs", false);
  g_st.nextIndex = g_fsPrefs.getULong("idx", 0);
  g_st.ackedThrough = (int32_t)g_fsPrefs.getLong("ack", -1);

  if (!LittleFS.exists(FS_DIR)) LittleFS.mkdir(FS_DIR);
  scanDir();

  Serial.printf("[fs] mounted %lu/%lu B, %u files (%u open), next index %lu, "
                "acked through %ld\n",
                (unsigned long)LittleFS.usedBytes(),
                (unsigned long)LittleFS.totalBytes(),
                g_fileCount, g_st.openFiles,
                (unsigned long)g_st.nextIndex, (long)g_st.ackedThrough);
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

void filestoreLoop() {
  if (!g_st.mounted || !g_enabled) return;

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
  if (!g_idleClosed && g_lastBusMs &&
      (uint32_t)(now - g_lastBusMs) >= CAN_BUS_IDLE_CLOSE_MS &&
      (g_actSnapshot.open || g_actChanges.open)) {
    const uint32_t a = g_actSnapshot.bytes, b = g_actChanges.bytes;
    filestoreCloseActive();
    g_idleClosed = true;
    Serial.printf("[fs] bus idle %lums -> closed all files "
                  "(tierB=%lu B, tierA=%lu B); safe to lose power\n",
                  (unsigned long)(now - g_lastBusMs),
                  (unsigned long)a, (unsigned long)b);
  }

  // Bound what a power cut or a panic can cost. Both tiers, every pass.
  flushDue(g_actSnapshot, now);
  flushDue(g_actChanges, now);

  g_st.rowsDropped = recorderChangeDropped();

  static uint32_t lastCheck = 0;
  if (now - lastCheck >= 5000) {
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
  // Either arm is sufficient, and the second one is the fix from retention
  // run 2: once anything unacked has been destroyed the warning is true
  // forever, regardless of what usage happens to be now. A latched flag would
  // be equivalent -- deletedUnacked already only ever counts up, so it IS the
  // latch, and keeping one source of truth means the two cannot disagree.
  return filestoreUsagePct() >= FS_WARN_USAGE_PCT || g_st.deletedUnacked > 0;
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
  // which is a heap spike this board should not take for a listing.
  srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
  srv.send(200, "application/json", "");

  char buf[256];
  srv.sendContent("[");
  for (uint16_t i = 0; i < g_fileCount; i++) {
    const FileEntry &e = g_files[i];
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
    esp_task_wdt_reset();
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

  enforceRetention();

  char buf[160];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"acked_through\":%ld,\"requested\":%ld,"
           "\"files\":%u,\"usage_pct\":%u}",
           (long)now, through, g_fileCount, filestoreUsagePct());
  srv.send(200, "application/json", buf);
}

void filestoreRegister(WebServer &srv) {
  srv.on("/api/v1/files", HTTP_GET, [&srv]() { handleList(srv); });
  srv.on("/api/v1/files/ack", HTTP_POST, [&srv]() { handleAck(srv); });
  // Registered AFTER /ack so the literal route wins; UriBraces would otherwise
  // match "ack" as an index and quietly 404 every acknowledgement.
  srv.on(UriBraces("/api/v1/files/{}"), HTTP_GET, [&srv]() { handleFetch(srv); });
}

// The filestore index and retention policy, tested on the host against a
// fake disk.
//
// 🐛 THE FAILURE (measured 2026-09-25 on the 200-cycle soak image): 1,376
// directory entries, a fixed 96-slot index, and scanDir() doing `continue`
// when addEntry() returned nullptr. 594 files were dropped from the index --
// invisible to retention, so never evicted, so the directory only grew.
//
// These tests drive the REAL fsScanFeed() and fsEnforceRetention(), with the
// disk and the loss accounting supplied as callbacks, and assert the two
// properties that failed:
//   * no file on disk is ever missing from the index, silently or otherwise
//   * the file count, as well as the bytes, ends up bounded
// and one bookkeeping identity that makes "silently" checkable:
//   files on disk at start == files left + evicted acked + evicted unacked
// with every unacked eviction counted as a loss.

#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fsindex.h"

// ---------------------------------------------------------------------------
// Fake disk
// ---------------------------------------------------------------------------

#define DISK_MAX 4096

struct FakeFile {
  char     name[40];
  uint32_t bytes;
  bool     present;
};

struct FakeDisk {
  FakeFile f[DISK_MAX];
  uint32_t n;
  // Accounting the firmware does in noteUnackedEviction() / deletedAcked.
  uint32_t lostFiles;
  uint64_t lostBytes;
  uint32_t deletedAcked;
  uint32_t removeCalls;
  uint32_t hydrateCalls;
};

static FakeDisk g_disk;

static void diskReset() { memset(&g_disk, 0, sizeof(g_disk)); }

static void diskAdd(const char *name, uint32_t bytes) {
  FakeFile &f = g_disk.f[g_disk.n++];
  snprintf(f.name, sizeof(f.name), "%s", name);
  f.bytes = bytes;
  f.present = true;
}

// One closed file = .log + .meta, like closeActive() leaves it.
static void diskAddClosed(uint32_t index, char kind, uint32_t bytes) {
  char n[40];
  snprintf(n, sizeof(n), "%06lu_%c%c_%08lX.log", (unsigned long)index,
           fsTierForKind(kind), kind, 0x2Aul);
  diskAdd(n, bytes);
  snprintf(n, sizeof(n), "%06lu_%c%c_%08lX.meta", (unsigned long)index,
           fsTierForKind(kind), kind, 0x2Aul);
  diskAdd(n, 110);
}

static void diskAddPart(uint32_t index, char kind, uint32_t bytes) {
  char n[40];
  snprintf(n, sizeof(n), "%06lu_%c%c_%08lX.part", (unsigned long)index,
           fsTierForKind(kind), kind, 0x29ul);
  diskAdd(n, bytes);
}

static FakeFile *diskFind(uint32_t index, bool closed) {
  for (uint32_t i = 0; i < g_disk.n; i++) {
    FakeFile &f = g_disk.f[i];
    if (!f.present) continue;
    FsName nm;
    if (!fsParseName(f.name, &nm)) continue;
    if (nm.index != index) continue;
    if (closed ? nm.ext == FS_EXT_LOG : nm.ext == FS_EXT_PART) return &f;
  }
  return nullptr;
}

// Files in the protocol's sense: .log and .part still on disk.
static uint32_t diskFileCount() {
  uint32_t c = 0;
  for (uint32_t i = 0; i < g_disk.n; i++) {
    FsName nm;
    if (g_disk.f[i].present && fsParseName(g_disk.f[i].name, &nm) &&
        nm.ext != FS_EXT_META) c++;
  }
  return c;
}

static uint32_t diskUsed() {
  uint32_t s = 0;
  for (uint32_t i = 0; i < g_disk.n; i++)
    if (g_disk.f[i].present) s += g_disk.f[i].bytes;
  return s;
}

// Deletes the data file AND its .meta, like removeFiles().
static void diskRemoveEntry(const FsEntry *e) {
  for (uint32_t i = 0; i < g_disk.n; i++) {
    FakeFile &f = g_disk.f[i];
    FsName nm;
    if (f.present && fsParseName(f.name, &nm) && nm.index == e->index)
      f.present = false;
  }
}

// --- the callbacks the firmware supplies -----------------------------------

static void opHydrate(void *, FsEntry *e) {
  g_disk.hydrateCalls++;
  FakeFile *f = diskFind(e->index, e->closed);
  e->bytes = f ? f->bytes : 0;
  e->hydrated = true;
}

static void opRemove(void *, const FsEntry *e, bool acked) {
  g_disk.removeCalls++;
  diskRemoveEntry(e);
  if (acked) {
    g_disk.deletedAcked++;
  } else {
    g_disk.lostFiles++;
    g_disk.lostBytes += e->bytes;
  }
}

static uint32_t opUsed(void *) { return diskUsed(); }

static const FsRetentionOps OPS = {nullptr, opHydrate, opRemove, opUsed};

// --- allocators --------------------------------------------------------------

static void *growOk(void *old, size_t, size_t newBytes) {
  return realloc(old, newBytes);
}

// Grows to at most `g_growLimit` entries, then refuses: a PSRAM allocation
// that fails partway through a scan.
static uint32_t g_growLimit = 0;
static void *growCapped(void *old, size_t, size_t newBytes) {
  if (newBytes / sizeof(FsEntry) > g_growLimit) return nullptr;
  return realloc(old, newBytes);
}

// --- the scan, as filestore.cpp runs it --------------------------------------

static FsScanStats scanDisk(FsTable *t) {
  FsScanStats st;
  memset(&st, 0, sizeof(st));
  for (uint32_t i = 0; i < g_disk.n; i++) {
    if (!g_disk.f[i].present) continue;
    FsEntry ev;
    if (fsScanFeed(t, g_disk.f[i].name, &st, &ev) == FS_SCAN_EVICT) {
      // What filestore.cpp does with a scan-time eviction: hydrate, delete,
      // account. Never just forget it.
      opHydrate(nullptr, &ev);
      opRemove(nullptr, &ev, false);
    }
  }
  fsTableSort(t);
  return st;
}

static void hydrateAll(FsTable *t) {
  for (uint16_t i = 0; i < t->count; i++)
    if (!t->v[i].hydrated) opHydrate(nullptr, &t->v[i]);
}

// Every file on disk is in the index, and every index entry is on disk.
static void assertIndexMatchesDisk(FsTable *t) {
  TEST_ASSERT_EQUAL_UINT32(diskFileCount(), t->count);
  for (uint16_t i = 0; i < t->count; i++) {
    TEST_ASSERT_NOT_NULL_MESSAGE(diskFind(t->v[i].index, t->v[i].closed),
                                 "index entry with no file on disk");
  }
}

static const int32_t NOTHING_ACKED = -1;

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

void test_names_parse_and_classify(void) {
  FsName n;
  TEST_ASSERT_TRUE(fsParseName("000123_Bs_0000002A.log", &n));
  TEST_ASSERT_EQUAL_UINT32(123, n.index);
  TEST_ASSERT_EQUAL_UINT32(0x2A, n.bootId);
  TEST_ASSERT_EQUAL_CHAR('s', n.kind);
  TEST_ASSERT_EQUAL_UINT8(FS_EXT_LOG, n.ext);
  TEST_ASSERT_TRUE(fsParseName("000124_Ac_0000002B.part", &n));
  TEST_ASSERT_EQUAL_UINT8(FS_EXT_PART, n.ext);
  TEST_ASSERT_TRUE(fsParseName("000124_Ac_0000002B.meta", &n));
  TEST_ASSERT_EQUAL_UINT8(FS_EXT_META, n.ext);
}

void test_foreign_names_are_rejected(void) {
  FsName n;
  TEST_ASSERT_FALSE(fsParseName("readme.txt", &n));
  TEST_ASSERT_FALSE(fsParseName("000123_Bx_0000002A.log", &n));   // bad kind
  TEST_ASSERT_FALSE(fsParseName("000123_Bs_0000002A.tmp", &n));   // bad ext
  TEST_ASSERT_FALSE(fsParseName("000123_Bs_0000002A.log.bak", &n));
  TEST_ASSERT_FALSE(fsParseName("", &n));
}

// ---------------------------------------------------------------------------
// f. The regression: more files than the old table, none dropped silently
// ---------------------------------------------------------------------------

void test_scan_of_the_soak_shape_keeps_every_file(void) {
  // The measured image: 686 .log, 686 .meta, 4 .part, no 0-byte files.
  diskReset();
  for (uint32_t i = 0; i < 686; i++)
    diskAddClosed(i, (i % 2) ? FS_KIND_SNAPSHOT : FS_KIND_CHANGES, 5000);
  for (uint32_t i = 686; i < 690; i++) diskAddPart(i, FS_KIND_SNAPSHOT, 4000);

  FsTable t;
  fsTableInit(&t, growOk);
  const FsScanStats st = scanDisk(&t);

  TEST_ASSERT_EQUAL_UINT32(1376, st.entries);
  TEST_ASSERT_EQUAL_UINT32(690, st.files);
  TEST_ASSERT_EQUAL_UINT32(686, st.metas);
  TEST_ASSERT_EQUAL_UINT32(0, st.evictedForRoom);
  TEST_ASSERT_EQUAL_UINT32(690, t.count);          // was 96
  assertIndexMatchesDisk(&t);
  // Linear: the scan read nothing but names.
  TEST_ASSERT_EQUAL_UINT32(0, g_disk.hydrateCalls);
  free(t.v);
}

void test_retention_bounds_the_count_and_accounts_every_eviction(void) {
  diskReset();
  const uint32_t N = 300, CAP = 96;
  for (uint32_t i = 0; i < N; i++)
    diskAddClosed(i, (i % 3) ? FS_KIND_SNAPSHOT : FS_KIND_CHANGES, 5000);
  const uint32_t before = diskFileCount();

  FsTable t;
  fsTableInit(&t, growOk);
  scanDisk(&t);
  TEST_ASSERT_EQUAL_UINT32(N, t.count);

  // Byte limits far away, so the COUNT cap alone has to do this.
  const FsRetentionCfg cfg = {CAP, 0xFFFFFFFFu, 0xFFFFFFFFu};
  FsRetentionResult r;
  uint32_t passes = 0;
  do {
    r = fsEnforceRetention(&t, NOTHING_ACKED, &cfg, &OPS, 0, 8);
    passes++;
  } while (r.more && passes < 1000);

  TEST_ASSERT_FALSE(r.more);
  TEST_ASSERT_FALSE(r.stuck);
  TEST_ASSERT_EQUAL_UINT32(CAP, t.count);
  assertIndexMatchesDisk(&t);
  // Nothing vanished without a record: every missing file is a counted loss.
  TEST_ASSERT_EQUAL_UINT32(before, diskFileCount() + g_disk.lostFiles +
                                   g_disk.deletedAcked);
  TEST_ASSERT_EQUAL_UINT32(N - CAP, g_disk.lostFiles);
  TEST_ASSERT_EQUAL_UINT64((uint64_t)(N - CAP) * 5000u, g_disk.lostBytes);
  // Per-pass budget respected: >= ceil(204 / 8) passes, not one long call.
  TEST_ASSERT_TRUE(passes >= (N - CAP + 7) / 8);
  free(t.v);
}

void test_count_eviction_follows_the_protocol_order(void) {
  // Oldest acked first, then unacked Tier A, then Tier B.
  diskReset();
  diskAddClosed(1, FS_KIND_SNAPSHOT, 100);   // acked
  diskAddClosed(2, FS_KIND_SNAPSHOT, 100);   // unacked B
  diskAddClosed(3, FS_KIND_CHANGES, 100);    // unacked A
  diskAddClosed(4, FS_KIND_CHANGES, 100);    // unacked A
  FsTable t;
  fsTableInit(&t, growOk);
  scanDisk(&t);

  const FsRetentionCfg cfg = {1, 0xFFFFFFFFu, 0xFFFFFFFFu};
  fsEnforceRetention(&t, /*ackedThrough=*/1, &cfg, &OPS, 0, 100);
  TEST_ASSERT_EQUAL_UINT16(1, t.count);
  // #1 went free (acked), #3 and #4 (Tier A) before #2 (Tier B).
  TEST_ASSERT_EQUAL_UINT32(2, t.v[0].index);
  TEST_ASSERT_EQUAL_UINT32(1, g_disk.deletedAcked);
  TEST_ASSERT_EQUAL_UINT32(2, g_disk.lostFiles);
  free(t.v);
}

void test_bytes_and_count_both_end_bounded(void) {
  diskReset();
  for (uint32_t i = 0; i < 200; i++)
    diskAddClosed(i, (i % 2) ? FS_KIND_SNAPSHOT : FS_KIND_CHANGES,
                  (i % 5 == 0) ? 60000 : 3000);
  FsTable t;
  fsTableInit(&t, growOk);
  scanDisk(&t);
  hydrateAll(&t);

  const FsRetentionCfg cfg = {96, 200000, 1000000};
  FsRetentionResult r;
  do { r = fsEnforceRetention(&t, NOTHING_ACKED, &cfg, &OPS, 0, 16); } while (r.more);

  TEST_ASSERT_TRUE(t.count <= 96);
  TEST_ASSERT_TRUE(fsTierABytes(&t) <= 200000);
  TEST_ASSERT_TRUE(diskUsed() <= 1000000);
  assertIndexMatchesDisk(&t);
  free(t.v);
}

void test_reserve_leaves_room_for_the_next_open(void) {
  diskReset();
  for (uint32_t i = 0; i < 10; i++) diskAddClosed(i, FS_KIND_SNAPSHOT, 100);
  FsTable t;
  fsTableInit(&t, growOk);
  scanDisk(&t);
  const FsRetentionCfg cfg = {10, 0xFFFFFFFFu, 0xFFFFFFFFu};
  fsEnforceRetention(&t, NOTHING_ACKED, &cfg, &OPS, /*reserve=*/1, 100);
  TEST_ASSERT_EQUAL_UINT16(9, t.count);
  free(t.v);
}

// ---------------------------------------------------------------------------
// Growth failure: evict, never drop
// ---------------------------------------------------------------------------

void test_growth_failure_evicts_oldest_with_loss_accounting(void) {
  diskReset();
  const uint32_t N = 200;
  // Newest first, so readdir order is the worst case for "oldest-first".
  for (uint32_t i = N; i-- > 0;) diskAddClosed(i, FS_KIND_SNAPSHOT, 1000);
  const uint32_t before = diskFileCount();

  g_growLimit = 64;                 // the "PSRAM" gives out at 64 entries
  FsTable t;
  fsTableInit(&t, growCapped);
  const FsScanStats st = scanDisk(&t);

  TEST_ASSERT_EQUAL_UINT16(64, t.count);
  TEST_ASSERT_EQUAL_UINT32(N - 64, st.evictedForRoom);
  assertIndexMatchesDisk(&t);
  // The survivors are the NEWEST 64, whatever order they arrived in.
  for (uint16_t i = 0; i < t.count; i++)
    TEST_ASSERT_TRUE(t.v[i].index >= N - 64);
  // And nothing vanished uncounted.
  TEST_ASSERT_EQUAL_UINT32(before, diskFileCount() + g_disk.lostFiles);
  free(t.v);
}

void test_growth_failure_with_acked_victims_is_not_a_loss(void) {
  // Scan-time eviction goes through the same accounting, which knows acked
  // from unacked -- only the caller's `acked` flag differs. Pinned here so a
  // refactor cannot start counting free deletions as losses.
  diskReset();
  for (uint32_t i = 0; i < 4; i++) diskAddClosed(i, FS_KIND_SNAPSHOT, 10);
  FsTable t;
  fsTableInit(&t, growOk);
  scanDisk(&t);
  const FsRetentionCfg cfg = {2, 0xFFFFFFFFu, 0xFFFFFFFFu};
  fsEnforceRetention(&t, /*ackedThrough=*/10, &cfg, &OPS, 0, 100);
  TEST_ASSERT_EQUAL_UINT32(2, g_disk.deletedAcked);
  TEST_ASSERT_EQUAL_UINT32(0, g_disk.lostFiles);
  free(t.v);
}

// ---------------------------------------------------------------------------
// Oddly shaped disks
// ---------------------------------------------------------------------------

void test_zero_byte_files_and_junk_are_handled(void) {
  diskReset();
  diskAddClosed(1, FS_KIND_SNAPSHOT, 0);     // 0-byte closed file
  diskAddPart(2, FS_KIND_CHANGES, 0);        // 0-byte crash artifact
  diskAdd("000003_Bs_0000002A.meta", 90);    // orphan .meta
  diskAdd("notes.txt", 5);                   // not ours
  FsTable t;
  fsTableInit(&t, growOk);
  const FsScanStats st = scanDisk(&t);
  TEST_ASSERT_EQUAL_UINT32(2, t.count);
  TEST_ASSERT_EQUAL_UINT32(1, st.unparsed);
  TEST_ASSERT_EQUAL_UINT32(2, st.metas);

  // A 0-byte file still counts against the COUNT cap and is still evictable.
  const FsRetentionCfg cfg = {0, 0xFFFFFFFFu, 0xFFFFFFFFu};
  const FsRetentionResult r = fsEnforceRetention(&t, NOTHING_ACKED, &cfg, &OPS, 0, 100);
  TEST_ASSERT_EQUAL_UINT16(0, t.count);
  TEST_ASSERT_FALSE(r.stuck);
  free(t.v);
}

void test_stale_part_is_evictable_but_active_never_is(void) {
  diskReset();
  diskAddPart(1, FS_KIND_SNAPSHOT, 100);     // crash artifact, older boot
  diskAddPart(2, FS_KIND_CHANGES, 100);      // will be marked active
  FsTable t;
  fsTableInit(&t, growOk);
  scanDisk(&t);
  fsTableFind(&t, 2)->active = true;

  const FsRetentionCfg cfg = {0, 0xFFFFFFFFu, 0xFFFFFFFFu};
  const FsRetentionResult r = fsEnforceRetention(&t, NOTHING_ACKED, &cfg, &OPS, 0, 100);
  TEST_ASSERT_EQUAL_UINT16(1, t.count);
  TEST_ASSERT_EQUAL_UINT32(2, t.v[0].index);  // the active one survived
  TEST_ASSERT_TRUE(r.stuck);                  // and the policy says why
  free(t.v);
}

void test_duplicate_log_and_part_keep_the_log(void) {
  diskReset();
  diskAddPart(5, FS_KIND_SNAPSHOT, 100);
  diskAddClosed(5, FS_KIND_SNAPSHOT, 100);
  FsTable t;
  fsTableInit(&t, growOk);
  const FsScanStats st = scanDisk(&t);
  TEST_ASSERT_EQUAL_UINT16(1, t.count);
  TEST_ASSERT_TRUE(t.v[0].closed);
  TEST_ASSERT_EQUAL_UINT32(1, st.duplicates);
  free(t.v);
}

void test_unhydrated_victim_is_hydrated_before_it_is_counted(void) {
  // The loss record must carry real bytes, not the zero an unread entry has.
  diskReset();
  diskAddClosed(1, FS_KIND_SNAPSHOT, 4321);
  FsTable t;
  fsTableInit(&t, growOk);
  scanDisk(&t);
  TEST_ASSERT_FALSE(t.v[0].hydrated);
  const FsRetentionCfg cfg = {0, 0xFFFFFFFFu, 0xFFFFFFFFu};
  fsEnforceRetention(&t, NOTHING_ACKED, &cfg, &OPS, 0, 100);
  TEST_ASSERT_EQUAL_UINT64(4321, g_disk.lostBytes);
  free(t.v);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_names_parse_and_classify);
  RUN_TEST(test_foreign_names_are_rejected);
  RUN_TEST(test_scan_of_the_soak_shape_keeps_every_file);
  RUN_TEST(test_retention_bounds_the_count_and_accounts_every_eviction);
  RUN_TEST(test_count_eviction_follows_the_protocol_order);
  RUN_TEST(test_bytes_and_count_both_end_bounded);
  RUN_TEST(test_reserve_leaves_room_for_the_next_open);
  RUN_TEST(test_growth_failure_evicts_oldest_with_loss_accounting);
  RUN_TEST(test_growth_failure_with_acked_victims_is_not_a_loss);
  RUN_TEST(test_zero_byte_files_and_junk_are_handled);
  RUN_TEST(test_stale_part_is_evictable_but_active_never_is);
  RUN_TEST(test_duplicate_log_and_part_keep_the_log);
  RUN_TEST(test_unhydrated_victim_is_hydrated_before_it_is_counted);
  return UNITY_END();
}

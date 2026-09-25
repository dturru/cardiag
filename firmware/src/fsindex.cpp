// The filestore's index and retention policy. No I/O -- see fsindex.h.
//
// Compiles against stdint + string.h + stdio.h alone, so it runs in
// `pio test -e native`.

#include "fsindex.h"

#include <stdio.h>
#include <string.h>

bool fsParseName(const char *base, FsName *out) {
  if (!base || !out) return false;
  // NNNNNN_TK_BBBBBBBB.ext -- fixed width, so check the shape by hand rather
  // than trusting sscanf's field widths to reject a near miss.
  unsigned long idx = 0, boot = 0;
  char t = 0, k = 0;
  char e[8] = {0};
  int consumed = 0;
  if (sscanf(base, "%6lu_%c%c_%8lX.%7s%n", &idx, &t, &k, &boot, e, &consumed) != 5)
    return false;
  if (base[consumed] != 0) return false;       // trailing junk: not ours
  if (k != FS_KIND_RAW && k != FS_KIND_CHANGES &&
      k != FS_KIND_SNAPSHOT && k != FS_KIND_BOOKEND) return false;

  FsExt ext = FS_EXT_NONE;
  if (strcmp(e, "log") == 0)       ext = FS_EXT_LOG;
  else if (strcmp(e, "part") == 0) ext = FS_EXT_PART;
  else if (strcmp(e, "meta") == 0) ext = FS_EXT_META;
  else return false;

  out->index = (uint32_t)idx;
  out->bootId = (uint32_t)boot;
  out->kind = k;
  out->tierInName = t;
  out->ext = ext;
  return true;
}

// ---------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------

void fsTableInit(FsTable *t, FsGrowFn grow) {
  t->v = nullptr;
  t->count = 0;
  t->cap = 0;
  t->grow = grow;
}

FsEntry *fsTableFind(FsTable *t, uint32_t index) {
  for (uint16_t i = 0; i < t->count; i++) {
    if (t->v[i].index == index) return &t->v[i];
  }
  return nullptr;
}

static bool growTable(FsTable *t) {
  if (!t->grow) return false;
  if (t->cap >= FS_TABLE_HARD_MAX) return false;
  // Doubling: amortised linear over a scan of any size.
  uint32_t want = t->cap ? (uint32_t)t->cap * 2u : 32u;
  if (want > FS_TABLE_HARD_MAX) want = FS_TABLE_HARD_MAX;
  void *p = t->grow(t->v, (size_t)t->cap * sizeof(FsEntry),
                    (size_t)want * sizeof(FsEntry));
  if (!p) return false;
  t->v = (FsEntry *)p;
  t->cap = (uint16_t)want;
  return true;
}

FsEntry *fsTableAdd(FsTable *t, uint32_t index) {
  FsEntry *e = fsTableFind(t, index);
  if (e) return e;
  if (t->count >= t->cap && !growTable(t)) return nullptr;
  e = &t->v[t->count++];
  memset(e, 0, sizeof(*e));
  e->index = index;
  return e;
}

void fsTableSort(FsTable *t) {
  // Insertion sort: nearly sorted in steady state (one new file at the end).
  // The boot scan sorts once, and readdir order is creation order on LittleFS
  // in practice, so this stays close to linear there too.
  for (uint16_t i = 1; i < t->count; i++) {
    FsEntry tmp = t->v[i];
    int32_t j = (int32_t)i - 1;
    while (j >= 0 && t->v[j].index > tmp.index) {
      t->v[j + 1] = t->v[j];
      j--;
    }
    t->v[j + 1] = tmp;
  }
}

void fsTableDrop(FsTable *t, uint16_t i) {
  if (i >= t->count) return;
  memmove(&t->v[i], &t->v[i + 1], (size_t)(t->count - i - 1) * sizeof(FsEntry));
  t->count--;
}

FsEntry *fsTableAddOrEvict(FsTable *t, const FsEntry *incoming,
                           FsEntry *evicted, bool *didEvict) {
  *didEvict = false;
  FsEntry *e = fsTableAdd(t, incoming->index);
  if (e) {
    *e = *incoming;
    return e;
  }

  // Full and cannot grow. Oldest non-active entry in the table...
  uint16_t oldest = t->count;
  for (uint16_t i = 0; i < t->count; i++) {
    if (t->v[i].active) continue;
    if (oldest == t->count || t->v[i].index < t->v[oldest].index) oldest = i;
  }
  *didEvict = true;
  // ...against the newcomer. The older of the two loses its slot.
  if (oldest == t->count || incoming->index < t->v[oldest].index) {
    *evicted = *incoming;
    return nullptr;
  }
  *evicted = t->v[oldest];
  t->v[oldest] = *incoming;
  return &t->v[oldest];
}

FsScanResult fsScanFeed(FsTable *t, const char *base, FsScanStats *st,
                        FsEntry *evicted) {
  st->entries++;
  FsName n;
  if (!fsParseName(base, &n)) {
    st->unparsed++;
    return FS_SCAN_SKIP;
  }
  if (n.ext == FS_EXT_META) {
    st->metas++;
    return FS_SCAN_SKIP;          // read via its .log, lazily
  }
  st->files++;

  // A .log and a .part with the same index: the rename half-happened. The
  // .log is the complete one; keep it and let the .part be the orphan.
  if (FsEntry *have = fsTableFind(t, n.index)) {
    st->duplicates++;
    if (have->closed || n.ext != FS_EXT_LOG) return FS_SCAN_ADDED;
    have->closed = true;
    have->hydrated = false;
    return FS_SCAN_ADDED;
  }

  FsEntry in;
  memset(&in, 0, sizeof(in));
  in.index = n.index;
  in.bootId = n.bootId;
  in.kind = n.kind;
  in.closed = (n.ext == FS_EXT_LOG);
  in.mode = FS_MODE_UNKNOWN;      // until hydrated
  in.active = false;              // nothing is open yet at boot
  in.hydrated = false;

  bool didEvict = false;
  fsTableAddOrEvict(t, &in, evicted, &didEvict);
  if (didEvict) {
    st->evictedForRoom++;
    return FS_SCAN_EVICT;
  }
  return FS_SCAN_ADDED;
}

// ---------------------------------------------------------------------------
// Retention
// ---------------------------------------------------------------------------

static inline bool isAcked(const FsEntry &e, int32_t ackedThrough) {
  return (int32_t)e.index <= ackedThrough;
}

uint32_t fsTierABytes(const FsTable *t) {
  uint32_t sum = 0;
  for (uint16_t i = 0; i < t->count; i++) {
    const FsEntry &e = t->v[i];
    if (e.hydrated && fsTierForKind(e.kind) == FS_TIER_RAW) sum += e.bytes;
  }
  return sum;
}

uint16_t fsPickVictim(const FsTable *t, int32_t ackedThrough) {
  // The table is kept sorted by index, so "first match" is "oldest".
  for (uint16_t i = 0; i < t->count; i++) {
    if (t->v[i].active) continue;
    if (isAcked(t->v[i], ackedThrough)) return i;
  }
  static const char kOrder[3] = {FS_TIER_RAW, FS_TIER_SNAPSHOT, FS_TIER_BOOKEND};
  for (uint8_t o = 0; o < 3; o++) {
    for (uint16_t i = 0; i < t->count; i++) {
      if (t->v[i].active) continue;
      if (fsTierForKind(t->v[i].kind) == kOrder[o]) return i;
    }
  }
  return t->count;
}

uint16_t fsPickTierAVictim(const FsTable *t, int32_t ackedThrough) {
  // Two passes on purpose -- see the note on oldestTierACandidate() that this
  // replaces: a single oldest-first sweep picks acked files by luck of
  // ordering, not by rule.
  for (int pass = 0; pass < 2; pass++) {
    const bool wantAcked = (pass == 0);
    for (uint16_t i = 0; i < t->count; i++) {
      const FsEntry &e = t->v[i];
      if (e.active) continue;
      if (fsTierForKind(e.kind) != FS_TIER_RAW) continue;
      if (isAcked(e, ackedThrough) == wantAcked) return i;
    }
  }
  return t->count;
}

static void evictAt(FsTable *t, uint16_t i, int32_t ackedThrough,
                    const FsRetentionOps *ops, FsRetentionResult *r) {
  FsEntry *e = &t->v[i];
  // The loss record must carry real bytes, so read them before deleting.
  if (!e->hydrated && ops->hydrate) ops->hydrate(ops->ctx, e);
  const bool acked = isAcked(*e, ackedThrough);
  ops->remove(ops->ctx, e, acked);
  fsTableDrop(t, i);
  r->evicted++;
  if (acked) r->evictedAcked++;
  else       r->evictedUnacked++;
}

FsRetentionResult fsEnforceRetention(FsTable *t, int32_t ackedThrough,
                                     const FsRetentionCfg *cfg,
                                     const FsRetentionOps *ops,
                                     uint16_t reserve, uint16_t budget) {
  FsRetentionResult r = {0, 0, 0, false, false};

  // 1. COUNT. Leave `reserve` slots so opening the next file cannot push the
  //    count back over the cap.
  const uint32_t countLimit =
      cfg->maxFiles > reserve ? cfg->maxFiles - reserve : 0;
  while (t->count > countLimit) {
    if (r.evicted >= budget) { r.more = true; return r; }
    const uint16_t i = fsPickVictim(t, ackedThrough);
    if (i == t->count) { r.stuck = true; break; }
    evictAt(t, i, ackedThrough, ops, &r);
  }

  // 2. TIER A SHARE. Tier A outruns Tier B ~50:1; ordering alone would let it
  //    fill the partition between two snapshot blocks.
  while (fsTierABytes(t) > cfg->tierACapBytes) {
    if (r.evicted >= budget) { r.more = true; return r; }
    const uint16_t i = fsPickTierAVictim(t, ackedThrough);
    if (i == t->count) break;          // only active Tier A left; leave it
    evictAt(t, i, ackedThrough, ops, &r);
  }

  // 3. USED BYTES. LittleFS needs slack to do anything, including delete.
  while (ops->usedBytes(ops->ctx) > cfg->usedLimitBytes) {
    if (r.evicted >= budget) { r.more = true; return r; }
    const uint16_t i = fsPickVictim(t, ackedThrough);
    if (i == t->count) { r.stuck = true; break; }
    evictAt(t, i, ackedThrough, ops, &r);
  }
  return r;
}

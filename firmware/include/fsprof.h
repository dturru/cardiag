#pragma once

// ---------------------------------------------------------------------------
// Where a filestore tick spends its time: EXCLUSIVE time per sub-stage.
//
// WHY (2026-09-25 soak, master 1c23efb). Every one of 40 cycles had a loop()
// pass of 1.2-2.4 s with stage "filestore" -- which names the function, not
// the cause. The tick writes the snapshot, drains the change log, flushes,
// opens/closes/renames, enforces retention and hydrates; any of them can
// reach flash. This splits the tick so the next soak names the culprit.
//
// EXCLUSIVE, because the stages nest: a snapshot write that has to rotate
// calls open, which calls retention, which asks LittleFS how full it is.
// Inclusive timing would charge all of that to "snapshot" and point at the
// wrong code. A stage's time here is its own, with every nested stage's time
// charged to that stage instead.
//
// Header-only, stdint only, clock injected: test/test_fsprof drives it.
// ---------------------------------------------------------------------------

#include <stdint.h>

enum FsSub : uint8_t {
  FS_SUB_SNAPSHOT = 0,   // building + writing the Tier B block
  FS_SUB_CHANGELOG,      // draining the Tier A change log to the file
  FS_SUB_FLUSH,          // fh.flush() on the time bound
  FS_SUB_ROTATE,         // open / close / .meta / rename / index add
  FS_SUB_RETENTION,      // eviction policy + removes
  FS_SUB_FSSIZE,         // a filesystem walk (usedBytes()/totalBytes() ->
                         // lfs_fs_size). Only the idle resync may walk in a
                         // tick now (fsusage.h); see `walks` below.
  FS_SUB_HYDRATE,        // stat / .meta reads for the index (scan tail)
  FS_SUB_COUNT
};

static inline const char *fsSubName(uint8_t s) {
  static const char *const names[FS_SUB_COUNT] = {
      "snapshot", "changelog", "flush", "rotate", "retention", "fs_size",
      "hydrate"};
  return s < FS_SUB_COUNT ? names[s] : "?";
}

#define FS_PROF_DEPTH 8

struct FsProf {
  uint32_t acc[FS_SUB_COUNT];       // exclusive us this pass
  uint8_t  stack[FS_PROF_DEPTH];
  uint32_t t0[FS_PROF_DEPTH];
  uint32_t child[FS_PROF_DEPTH];    // time spent in nested stages
  uint8_t  depth;
  bool     active;                  // inside a tick; outside, no-ops
  uint16_t walks;                   // filesystem walks this pass
};

// Count one filesystem walk (a usedBytes()/totalBytes() call). The number
// that says whether the fix holds: it should be 0 in every tick except the
// rare idle resync.
static inline void fsProfWalk(FsProf *p) {
  if (p->active) p->walks++;
}

static inline void fsProfBeginPass(FsProf *p) {
  for (uint8_t i = 0; i < FS_SUB_COUNT; i++) p->acc[i] = 0;
  p->depth = 0;
  p->walks = 0;
  p->active = true;
}

static inline void fsProfEnter(FsProf *p, uint8_t sub, uint32_t nowUs) {
  if (!p->active || p->depth >= FS_PROF_DEPTH) {
    // Too deep: count the depth anyway so the matching exit stays balanced.
    if (p->active) p->depth++;
    return;
  }
  p->stack[p->depth] = sub;
  p->t0[p->depth] = nowUs;
  p->child[p->depth] = 0;
  p->depth++;
}

static inline void fsProfExit(FsProf *p, uint32_t nowUs) {
  if (!p->active || p->depth == 0) return;
  p->depth--;
  if (p->depth >= FS_PROF_DEPTH) return;            // an over-deep frame
  const uint32_t d = p->depth;
  const uint32_t elapsed = nowUs - p->t0[d];
  const uint32_t self = elapsed > p->child[d] ? elapsed - p->child[d] : 0;
  p->acc[p->stack[d]] += self;
  if (d > 0) p->child[d - 1] += elapsed;
}

// The worst sub-stage of the pass just ended. Returns its index, and its us.
static inline uint8_t fsProfEndPass(FsProf *p, uint32_t *worstUs) {
  p->active = false;
  uint8_t w = 0;
  for (uint8_t i = 1; i < FS_SUB_COUNT; i++) {
    if (p->acc[i] > p->acc[w]) w = i;
  }
  *worstUs = p->acc[w];
  return w;
}

// Per-window worst, the same shape as looptime.h's windows: taken (and
// reset) by the session read and by the stats line independently.
struct FsSubWindow {
  uint32_t worstUs;       // worst single sub-stage in any one pass
  uint8_t  worstSub;
  uint32_t passUs;        // the whole tick in that same pass
  uint32_t walks;         // filesystem walks across ALL passes in the window
};

static inline void fsSubWindowAddWalks(FsSubWindow *w, uint16_t walks) {
  w->walks += walks;
}

static inline void fsSubWindowNote(FsSubWindow *w, uint8_t sub, uint32_t us,
                                   uint32_t passUs) {
  if (us > w->worstUs) {
    w->worstUs = us;
    w->worstSub = sub;
    w->passUs = passUs;
  }
}

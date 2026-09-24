#pragma once

// ---------------------------------------------------------------------------
// THE GO-TO-SLEEP INVARIANT
//
//   The go-to-sleep command may be issued ONLY when every file is closed.
//
// Why this is a guard in code rather than a timing margin: per the TCAN1043
// datasheet, INH does not drop on its own in normal mode. Sleep is
// MCU-commanded (EN high + nSTB low), and INH follows ~20-50 us later. So the
// firmware is not racing the hardware -- it IS the hardware's trigger. The
// only way an open file loses its bytes at key-off is if this firmware asks
// for the power to be cut while still holding one.
//
// That makes it an invariant, and invariants belong in an assertion rather
// than in a comment asking the next person to remember.
//
// ⚠️ ORDERING, and it is the whole point:
//      bus quiet -> CLOSE FILES -> command sleep -> INH drops -> power gone
//   not:
//      bus quiet -> command sleep -> ... -> hope the close finished
//
// Pure and header-only on purpose: filestore.cpp cannot compile on the host
// (LittleFS is ESP32-only), so putting the decision here is what lets the
// invariant be tested natively instead of only by unplugging a board.
// ---------------------------------------------------------------------------

#include <stdint.h>

enum SleepVerdict : uint8_t {
  SLEEP_OK = 0,
  // A file is still open. Commanding sleep now drops INH ~20-50 us later and
  // takes the 3.3 V rail with it, losing everything LittleFS has buffered.
  SLEEP_REFUSED_FILES_OPEN = 1,
  // The bus has not been quiet long enough to believe the trip is over.
  SLEEP_REFUSED_BUS_ACTIVE = 2,
  // Storage never mounted, so "no open files" is not evidence of anything.
  SLEEP_REFUSED_FS_UNKNOWN = 3,
};

// The decision, with no I/O so it can be tested.
//
// `openFiles`    how many files the filestore currently holds open
// `mounted`      whether the filestore mounted at all
// `busQuietMs`   milliseconds since the last received frame
// `quietNeedMs`  CAN_BUS_IDLE_CLOSE_MS
static inline SleepVerdict sleepVerdict(uint16_t openFiles, bool mounted,
                                        uint32_t busQuietMs,
                                        uint32_t quietNeedMs) {
  // Checked BEFORE the quiet test: an open file is a hard refusal no matter
  // how long the bus has been silent, and reporting the most serious reason
  // is what makes the log line useful.
  if (!mounted) return SLEEP_REFUSED_FS_UNKNOWN;
  if (openFiles > 0) return SLEEP_REFUSED_FILES_OPEN;
  if (busQuietMs < quietNeedMs) return SLEEP_REFUSED_BUS_ACTIVE;
  return SLEEP_OK;
}

static inline const char *sleepVerdictName(SleepVerdict v) {
  switch (v) {
    case SLEEP_OK:                     return "ok";
    case SLEEP_REFUSED_FILES_OPEN:     return "refused: files still open";
    case SLEEP_REFUSED_BUS_ACTIVE:     return "refused: bus still active";
    case SLEEP_REFUSED_FS_UNKNOWN:     return "refused: filestore not mounted";
    default:                           return "refused: unknown";
  }
}

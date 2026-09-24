#pragma once

// ---------------------------------------------------------------------------
// THE GO-TO-SLEEP INVARIANT, AND THE FAILSAFE WE HAVE TO PROVIDE OURSELVES
//
//   The go-to-sleep command may be issued ONLY when every file is closed.
//
// Per the TCAN1043xx-Q1 datasheet (covers the G), verified 2026-09-23:
//   * Sleep is MCU-COMMANDED ONLY. INH never drops by itself in normal mode.
//   * tGO_TO_SLEEP = 5-50 us after EN high + nSTB low.
//   * G vs H is CAN FD 5 Mbps (G) and +/-70 V vs +/-58 V bus fault. The
//     as-built carrier part is TCAN1043GDRQ1; +/-58 V is ample for a 12 V car
//     (see vault Carrier Board BOM §11.1).
//
// 🚨🚨 THE G HAS NO tINACTIVE / SWE FAILSAFE. The A variant does; ours does
// NOT. There is therefore NO hardware backstop that will ever turn this board
// off. INH sits on OBD pin 16, which is UNSWITCHED and permanently connected
// to the battery, so a firmware hang with INH still asserted is an ESP32 awake
// on the battery forever -- the exact failure INH exists to prevent, and the
// project's stated #1 constraint (an ESP32 awake with WiFi flattens a car
// battery in about a week).
//
// ⇒ WE OWN THE FAILSAFE. Three independent layers, because the one that
// matters is whichever one still works when the others are the thing that
// broke:
//
//   1. HARDWARE TASK WATCHDOG -- a hang resets the chip. Layers 2 and 3 are
//      code, so they cannot help if the code is what stopped running.
//   2. BOOT-TIME CHECK -- on every boot, if the bus is already quiet, close
//      any leftover files and sleep immediately. This is what catches the
//      reset that layer 1 just caused: waking up, finding a dead car, and
//      staying awake would convert a watchdog save into the same battery
//      drain.
//   3. MAX-AWAKE BACKSTOP -- bus quiet for CAN_MAX_AWAKE_MS and we sleep
//      regardless of whatever else the firmware thinks it is busy with.
//
// ⚠️ "Regardless" does NOT mean sleeping on top of an open file. At the
// backstop the answer is CLOSE THEN SLEEP: the invariant is preserved by
// closing the files, not by refusing to sleep. Refusing forever is the
// battery-flattening outcome, so it is not the safe default here -- which is
// the opposite of the normal case, and is why the backstop is a distinct
// verdict rather than a longer timeout.
//
// Pure and header-only on purpose: filestore.cpp cannot compile on the host
// (LittleFS is ESP32-only), so putting the decision here is what lets the
// invariant be tested natively instead of only by unplugging a board.
// ---------------------------------------------------------------------------

#include <stdint.h>

enum SleepVerdict : uint8_t {
  // Debounce satisfied, nothing open: command sleep.
  SLEEP_OK = 0,
  // A file is still open. Commanding sleep now drops INH 5-50 us later and
  // takes the 3.3 V rail with it, losing everything LittleFS has buffered.
  SLEEP_REFUSED_FILES_OPEN = 1,
  // The bus has not been quiet long enough to believe the trip is over.
  SLEEP_REFUSED_BUS_ACTIVE = 2,
  // Storage never mounted, so "no open files" is not evidence of anything.
  SLEEP_REFUSED_FS_UNKNOWN = 3,
  // ⭐ BACKSTOP. The bus has been quiet for CAN_MAX_AWAKE_MS and a file is
  // STILL open -- something is wrong. Close it and sleep anyway: there is no
  // hardware failsafe on the G, so staying awake to protect one file means
  // draining the battery instead.
  SLEEP_BACKSTOP_CLOSE_THEN_SLEEP = 4,
};

// The decision, with no I/O so it can be tested.
//
// `openFiles`    how many files the filestore currently holds open
// `mounted`      whether the filestore mounted at all
// `busQuietMs`   milliseconds since the last received frame
// `quietNeedMs`  CAN_BUS_IDLE_CLOSE_MS -- the normal debounce
// `maxAwakeMs`   CAN_MAX_AWAKE_MS -- the backstop; 0 disables it
static inline SleepVerdict sleepVerdict(uint16_t openFiles, bool mounted,
                                        uint32_t busQuietMs,
                                        uint32_t quietNeedMs,
                                        uint32_t maxAwakeMs) {
  // The backstop is tested FIRST and overrides every refusal below it. Past
  // this point the battery outranks the data: we still never sleep on top of
  // an open file, we close it first.
  if (maxAwakeMs && busQuietMs >= maxAwakeMs) {
    return openFiles > 0 ? SLEEP_BACKSTOP_CLOSE_THEN_SLEEP : SLEEP_OK;
  }
  if (!mounted) return SLEEP_REFUSED_FS_UNKNOWN;
  if (openFiles > 0) return SLEEP_REFUSED_FILES_OPEN;
  if (busQuietMs < quietNeedMs) return SLEEP_REFUSED_BUS_ACTIVE;
  return SLEEP_OK;
}

// True if the caller must close files before honouring this verdict.
static inline bool sleepNeedsClose(SleepVerdict v) {
  return v == SLEEP_BACKSTOP_CLOSE_THEN_SLEEP;
}

// True if the caller should end up asleep, with or without a close first.
static inline bool sleepShouldSleep(SleepVerdict v) {
  return v == SLEEP_OK || v == SLEEP_BACKSTOP_CLOSE_THEN_SLEEP;
}

static inline const char *sleepVerdictName(SleepVerdict v) {
  switch (v) {
    case SLEEP_OK:                 return "ok";
    case SLEEP_REFUSED_FILES_OPEN: return "refused: files still open";
    case SLEEP_REFUSED_BUS_ACTIVE: return "refused: bus still active";
    case SLEEP_REFUSED_FS_UNKNOWN: return "refused: filestore not mounted";
    case SLEEP_BACKSTOP_CLOSE_THEN_SLEEP:
      return "BACKSTOP: max awake reached with files open -- closing, then sleeping";
    default:                       return "refused: unknown";
  }
}

#include <Arduino.h>

#include "transceiver.h"
#include "filestore.h"
#include "power.h"
#include "config.h"

// TRANSCEIVER_HAS_INH (config.h): whether this build drives the carrier's
// TCAN1043 EN/nSTB. Off on a bare ESP32-CAN-X2, where nothing here can
// remove power, and saying so in one place beats a caller assuming it can.

bool transceiverHasInhPath() { return TRANSCEIVER_HAS_INH != 0; }

static XcvrMode g_mode = XCVR_STANDBY;

void transceiverBegin() {
#if TRANSCEIVER_HAS_INH
  // Same levels as the 10 k pull-downs: standby, INH on. Written before the
  // pins become outputs so they never glitch to anything else.
  digitalWrite(XCVR_EN_GPIO, LOW);
  digitalWrite(XCVR_NSTB_GPIO, LOW);
  pinMode(XCVR_EN_GPIO, OUTPUT);
  pinMode(XCVR_NSTB_GPIO, OUTPUT);
#endif
  g_mode = XCVR_STANDBY;
}

void transceiverSetMode(XcvrMode m) {
#if TRANSCEIVER_HAS_INH
  const XcvrPins p = xcvrPinsFor(m);
  if (m == XCVR_GO_TO_SLEEP) {
    // Go-to-sleep is entered from silent, so get there first (from standby
    // after a bus-wake boot). Then EN high, nSTB low -- the datasheet order;
    // the reverse lands in standby, which keeps INH on. The two writes are
    // back to back (sub-microsecond): EN=1/nSTB=1 exists only for that instant,
    // and with TXD unconnected (internal pull-up, recessive) it sends nothing.
    // ⚠️ Verify on a scope that INH falls; this is the one place it can.
    digitalWrite(XCVR_EN_GPIO, LOW);
    digitalWrite(XCVR_NSTB_GPIO, HIGH);
    delayMicroseconds(50);
    digitalWrite(XCVR_EN_GPIO, HIGH);
    digitalWrite(XCVR_NSTB_GPIO, LOW);
  } else {
    // silent/standby: nSTB first, so EN and nSTB are never both high.
    digitalWrite(XCVR_EN_GPIO, LOW);
    digitalWrite(XCVR_NSTB_GPIO, p.nstb ? HIGH : LOW);
  }
  if (m != g_mode)
    Serial.printf("[xcvr] TCAN1043 -> %s (EN=%d nSTB=%d)\n", xcvrModeName(m),
                  (int)p.en, (int)p.nstb);
#else
  (void)m;
#endif
  g_mode = m;
}

XcvrMode transceiverMode() { return g_mode; }

// --- the ordered sequence (sleepguard.h sleepExecute) -----------------------

static uint16_t ioOpenFiles(void *) { return filestoreStats()->openFiles; }
static void ioCloseAll(void *) { filestoreCloseActive(); }
static void ioAnnounceSafe(void *ctx) {
  // The bench power-cut test cuts power exactly HERE, so what it measures is
  // the real ordering rather than a convenient one.
  Serial.printf("[sleep] all files closed, %s quiet %lums -- "
                "SAFE TO CUT POWER NOW%s\n",
                powerPolicyName(powerPolicy()), (unsigned long)(uintptr_t)ctx,
                transceiverHasInhPath() ? "" : " (no INH path on this board)");
}
static void ioCommandSleep(void *) { transceiverSetMode(XCVR_GO_TO_SLEEP); }

static SleepVerdict execute(SleepVerdict v, uint32_t quiet) {
  const SleepIo io = {(void *)(uintptr_t)quiet, ioOpenFiles, ioCloseAll,
                      ioAnnounceSafe, ioCommandSleep};
  return sleepExecute(v, io);
}

SleepVerdict transceiverRequestSleep() {
  const FileStoreStats *fs = filestoreStats();
  // The policy picks the clock (bus silence, or ignition-off time); the guard
  // below is the same for both, so the open-file invariant is written once.
  const PowerThresholds t = powerThresholdsNow();
  const uint32_t quiet = powerQuietNowMs();
  const SleepVerdict v = sleepVerdict(fs->openFiles, fs->mounted, quiet,
                                      t.closeMs, t.sleepMs);

  // ⭐ BACKSTOP. The G variant has no tINACTIVE failsafe, so refusing forever
  // to protect an open file means draining the battery instead. Close it and
  // go -- the invariant is kept by closing, not by refusing.
  if (sleepNeedsClose(v)) {
    Serial.printf("[sleep] %s (open=%u, %s quiet=%lums)\n",
                  sleepVerdictName(v), (unsigned)fs->openFiles,
                  powerPolicyName(powerPolicy()), (unsigned long)quiet);
  }

  if (!sleepShouldSleep(v)) {
    // Loud, because a refusal here is the thing standing between an open file
    // and a power cut. It is also cheap: this is not a hot path.
    Serial.printf("[sleep] NOT commanding sleep -- %s "
                  "(open=%u mounted=%d %s quiet=%lums)\n",
                  sleepVerdictName(v), (unsigned)fs->openFiles,
                  (int)fs->mounted, powerPolicyName(powerPolicy()),
                  (unsigned long)quiet);
    return v;
  }

  const SleepVerdict done = execute(v, quiet);
  if (done == SLEEP_REFUSED_FILES_OPEN)
    Serial.printf("[sleep] NOT commanding sleep -- close left %u file(s) open\n",
                  (unsigned)fs->openFiles);
  return done;
}

void transceiverForceSleep(const char *why) {
  const uint32_t quiet = powerQuietNowMs();
  Serial.printf("[sleep] BACKSTOP: %s -- forcing close + go-to-sleep\n", why);
  if (execute(SLEEP_BACKSTOP_CLOSE_THEN_SLEEP, quiet) == SLEEP_REFUSED_FILES_OPEN)
    Serial.println("[sleep] BACKSTOP: close left a file open; NOT sleeping");
}

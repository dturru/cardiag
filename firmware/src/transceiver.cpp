#include <Arduino.h>

#include "transceiver.h"
#include "filestore.h"
#include "power.h"
#include "config.h"

// The ESP32-CAN-X2 has no INH path and no EN/nSTB control: those pins exist on
// the carrier board. Nothing here can remove power yet, and saying so in one
// place is better than a caller assuming it can.
#ifndef TRANSCEIVER_HAS_INH
#define TRANSCEIVER_HAS_INH 0
#endif

bool transceiverHasInhPath() { return TRANSCEIVER_HAS_INH != 0; }

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
    filestoreCloseActive();
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

  // ⭐ THE INSTANT THE RAIL WOULD DIE.
  //
  // On the carrier this is followed by EN high + nSTB low, and INH drops
  // ~20-50 us later. The bench power-cut test cuts power exactly HERE, so what
  // it measures is the real ordering rather than a convenient one.
  Serial.printf("[sleep] all files closed, %s quiet %lums -- "
                "SAFE TO CUT POWER NOW%s\n",
                powerPolicyName(powerPolicy()), (unsigned long)quiet,
                transceiverHasInhPath() ? "" : " (no INH path on this board)");

#if TRANSCEIVER_HAS_INH
  // Carrier board only. Order matters: EN high THEN nSTB low is what the
  // datasheet calls go-to-sleep; the reverse is standby, which does not drop
  // INH and would leave the board powered with the bus unbiased.
  digitalWrite(PIN_XCVR_EN, HIGH);
  digitalWrite(PIN_XCVR_NSTB, LOW);
#endif
  return SLEEP_OK;
}

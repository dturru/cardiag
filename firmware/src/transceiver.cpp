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

#if TRANSCEIVER_HAS_INH
#include <driver/gpio.h>

static bool g_pinsReady = false;

// ESP-IDF, not digitalWrite(): the 10-01 bench showed Arduino's pin manager
// rejecting IO38/IO39 ("not set as GPIO") while the log claimed the mode had
// changed. Here the output latch is written FIRST, then the pin becomes an
// input+output (so its level can be read back), so it never glitches: the
// 10 k pull-downs hold it low until then, and the latch already says low.
static bool pinSetup(int pin) {
  gpio_set_level((gpio_num_t)pin, 0);
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << pin;
  c.mode = GPIO_MODE_INPUT_OUTPUT;
  c.pull_up_en = GPIO_PULLUP_DISABLE;
  c.pull_down_en = GPIO_PULLDOWN_DISABLE;
  c.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&c) != ESP_OK) return false;
  return gpio_set_level((gpio_num_t)pin, 0) == ESP_OK &&
         gpio_get_level((gpio_num_t)pin) == 0;
}

static bool pinWrite(int pin, bool high) {
  return gpio_set_level((gpio_num_t)pin, high ? 1 : 0) == ESP_OK;
}

// Both pins read back at the levels `m` needs.
static bool pinsAre(XcvrPins p) {
  return gpio_get_level((gpio_num_t)XCVR_EN_GPIO) == (int)p.en &&
         gpio_get_level((gpio_num_t)XCVR_NSTB_GPIO) == (int)p.nstb;
}
#endif

void transceiverBegin() {
#if TRANSCEIVER_HAS_INH
  // Standby (EN=0, nSTB=0), the same levels as the pull-downs: INH on.
  g_pinsReady = pinSetup(XCVR_EN_GPIO) && pinSetup(XCVR_NSTB_GPIO);
  if (g_pinsReady)
    Serial.printf("[xcvr] TCAN1043 EN=GPIO%d nSTB=GPIO%d outputs, standby "
                  "(read back EN=0 nSTB=0)\n", XCVR_EN_GPIO, XCVR_NSTB_GPIO);
  else
    Serial.printf("[xcvr] TCAN1043 pin setup FAILED (EN=GPIO%d nSTB=GPIO%d): "
                  "mode changes will not be attempted\n",
                  XCVR_EN_GPIO, XCVR_NSTB_GPIO);
#endif
  g_mode = XCVR_STANDBY;
}

bool transceiverSetMode(XcvrMode m) {
#if TRANSCEIVER_HAS_INH
  if (!g_pinsReady) {
    Serial.printf("[xcvr] TCAN1043 -> %s NOT done: pins not set up\n",
                  xcvrModeName(m));
    return false;
  }
  const XcvrPins p = xcvrPinsFor(m);
  bool ok;
  if (m == XCVR_GO_TO_SLEEP) {
    // Go-to-sleep is entered from silent, so get there first (from standby
    // after a bus-wake boot). Then EN high, nSTB low -- the datasheet order;
    // the reverse lands in standby, which keeps INH on. The two writes are
    // back to back (sub-microsecond): EN=1/nSTB=1 exists only for that instant,
    // and with TXD unconnected (internal pull-up, recessive) it sends nothing.
    // ⚠️ Verify on a scope that INH falls; this is the one place it can.
    ok = pinWrite(XCVR_EN_GPIO, false) && pinWrite(XCVR_NSTB_GPIO, true);
    delayMicroseconds(50);
    ok = ok && pinWrite(XCVR_EN_GPIO, true) && pinWrite(XCVR_NSTB_GPIO, false);
  } else {
    // silent/standby: EN first, so EN and nSTB are never both high.
    ok = pinWrite(XCVR_EN_GPIO, false) && pinWrite(XCVR_NSTB_GPIO, p.nstb);
  }
  // Only a confirmed mode is reported, or recorded.
  ok = ok && pinsAre(p);
  if (!ok) {
    Serial.printf("[xcvr] TCAN1043 -> %s FAILED: read back EN=%d nSTB=%d, "
                  "wanted EN=%d nSTB=%d\n", xcvrModeName(m),
                  gpio_get_level((gpio_num_t)XCVR_EN_GPIO),
                  gpio_get_level((gpio_num_t)XCVR_NSTB_GPIO),
                  (int)p.en, (int)p.nstb);
    return false;
  }
  if (m != g_mode)
    Serial.printf("[xcvr] TCAN1043 -> %s (read back EN=%d nSTB=%d)\n",
                  xcvrModeName(m), (int)p.en, (int)p.nstb);
  g_mode = m;
  return true;
#else
  g_mode = m;
  return true;
#endif
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

#include <Arduino.h>
#include <esp_system.h>

#include "power.h"
#include "cantx.h"
#include "filestore.h"
#include "transceiver.h"
#include "config.h"
#include "logq.h"
#include <atomic>

static const PowerPolicy kPolicy = (PowerPolicy)POWER_POLICY;
static IgnDebounce g_ign = {};
static bool        g_seenOn = false;      // ignition ON at any point this boot
static bool        g_wdtBoot = false;     // this boot is a watchdog/panic reset
static WakeSource  g_wake = WAKE_NA;
// Bumped from canTask (OBD) and the selftest task: atomic.
static std::atomic<uint32_t> g_txBlocked{0};
static std::atomic<uint32_t> g_txBlockedLogMs{0};
// Gate open but the driver refused (queue full, bus-off): canrecov.h's input.
static std::atomic<uint32_t> g_txFailed{0};

static bool readIgnRaw() {
  const int v = digitalRead(IGN_SENSE_GPIO);
  return IGN_ACTIVE_HIGH ? v == HIGH : v == LOW;
}

// Ignition ON: hold the board's own supply with INH by keeping the TCAN1043
// in SILENT (EN low, nSTB high). Never normal: there is no TXD path.
static void onIgnitionOn() {
  g_seenOn = true;
  transceiverSetMode(XCVR_SILENT);
}

void powerBegin() {
  if (kPolicy != POWER_IGNITION) {
    Serial.printf("[power] policy=bus-quiet  files close after %ums of bus "
                  "silence, power-down after %lus\n",
                  (unsigned)CAN_BUS_IDLE_CLOSE_MS,
                  (unsigned long)(CAN_MAX_AWAKE_MS / 1000));
    return;
  }
  // Pull toward OFF (active-low: pull-up). The carrier also has a 10 k
  // pull-up; an unwired input therefore reads OFF and the board powers down.
  pinMode(IGN_SENSE_GPIO, IGN_ACTIVE_HIGH ? INPUT_PULLDOWN : INPUT_PULLUP);
  transceiverBegin();

  const esp_reset_reason_t rr = esp_reset_reason();
  g_wdtBoot = rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT ||
              rr == ESP_RST_WDT || rr == ESP_RST_PANIC;

  // One raw sample decides the wake source; the debouncer takes the first
  // sample as-is, so a board that boots with ignition off is off from boot.
  powerLoop();
  // HWCDC: true while a USB host is attached (setup() already waited ~2 s
  // for enumeration).
  g_wake = wakeSourceAtBoot(kPolicy, g_ign.on, (bool)Serial);
  if (g_ign.on) onIgnitionOn();

  const PowerThresholds t = powerThresholdsNow();
  Serial.printf("[power] policy=ignition  wake=%s  ignition %s  GPIO%d "
                "active-%s  (debounce on %ums / off %ums)\n",
                wakeSourceName(g_wake), g_ign.on ? "ON" : "OFF",
                IGN_SENSE_GPIO, IGN_ACTIVE_HIGH ? "high" : "low",
                (unsigned)IGN_ON_DEBOUNCE_MS, (unsigned)IGN_OFF_DEBOUNCE_MS);
  if (!g_ign.on)
    Serial.printf("[power] ignition OFF at boot%s: no TX, no recording, "
                  "go-to-sleep in %lums unless it comes on\n",
                  g_wdtBoot ? " after a watchdog/panic reset" : "",
                  (unsigned long)t.sleepMs);
}

void powerLoop() {
  if (kPolicy != POWER_IGNITION) return;
  const bool first = !g_ign.init;
  const bool was = g_ign.on;
  const bool on = ignDebounce(g_ign, readIgnRaw(), millis(),
                              IGN_ON_DEBOUNCE_MS, IGN_OFF_DEBOUNCE_MS);
  if (first || on == was) return;
  Serial.printf("[power] ignition %s\n", on ? "ON" : "OFF");
  if (on) onIgnitionOn();
}

PowerPolicy powerPolicy() { return kPolicy; }

PowerThresholds powerThresholdsNow() {
  if (kPolicy != POWER_IGNITION)
    return powerThresholds(kPolicy, CAN_BUS_IDLE_CLOSE_MS, CAN_MAX_AWAKE_MS,
                           IGN_OFF_DEBOUNCE_MS, IGN_OFF_SLEEP_MS);
  PowerThresholds t;
  t.closeMs = IGN_OFF_DEBOUNCE_MS ? IGN_OFF_DEBOUNCE_MS : 1;
  const uint32_t after = ignSleepAfterMs(g_seenOn, g_wdtBoot, IGN_OFF_SLEEP_MS,
                                         IGN_BUSWAKE_AWAKE_MS);
  // Seen ON: OFF time runs from the real edge and is >= closeMs when OFF is
  // believed. Never seen ON: OFF time runs from boot. Never 0 (0 = disabled).
  t.sleepMs = g_seenOn ? t.closeMs + after : after;
  if (!t.sleepMs) t.sleepMs = 1;
  return t;
}

uint32_t powerQuietNowMs() {
  return powerQuietMs(kPolicy, filestoreBusQuietMs(), g_ign, millis());
}

bool powerMayRecordNow() {
  if (kPolicy != POWER_IGNITION) return true;
  // Only with the ignition (debounced) ON: a bus-wake boot records nothing.
  return g_ign.on && powerMayRecord(kPolicy, powerQuietNowMs(), powerThresholdsNow());
}

bool powerIgnitionOn() { return kPolicy != POWER_IGNITION || g_ign.on; }

bool powerIgnitionOffPending(uint32_t *edgeMs) {
  if (kPolicy != POWER_IGNITION || !g_ign.init || !g_ign.on || !g_ign.pending)
    return false;
  if (edgeMs) *edgeMs = g_ign.edgeMs;
  return true;
}
WakeSource powerWakeSource() { return g_wake; }

// ---------------------------------------------------------------------------
// The TX gate (cantx.h).
// ---------------------------------------------------------------------------
bool canTxAllowed() { return canTxGate(kPolicy, powerIgnitionOn()); }

esp_err_t canTransmit(const twai_message_t *msg, TickType_t ticksToWait) {
  if (!canTxAllowed()) {
    const uint32_t n = ++g_txBlocked;
    const uint32_t now = millis();
    if (now - g_txBlockedLogMs.load() >= 5000) {
      g_txBlockedLogMs.store(now);
      logqPrintf("[power] TX refused: ignition off (%lu refused)\n",
                 (unsigned long)n);
    }
    return ESP_ERR_INVALID_STATE;
  }
  const esp_err_t err = twai_transmit(msg, ticksToWait);
  if (err != ESP_OK) ++g_txFailed;
  return err;
}

uint32_t canTxBlocked() { return g_txBlocked.load(); }
uint32_t canTxFailed()  { return g_txFailed.load(); }

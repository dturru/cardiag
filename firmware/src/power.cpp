#include <Arduino.h>

#include "power.h"
#include "filestore.h"
#include "config.h"

static const PowerPolicy kPolicy = (PowerPolicy)POWER_POLICY;
static IgnDebounce g_ign = {};

static bool readIgnRaw() {
  const int v = digitalRead(IGN_SENSE_GPIO);
  return IGN_ACTIVE_HIGH ? v == HIGH : v == LOW;
}

void powerBegin() {
  if (kPolicy == POWER_IGNITION) {
    // Pull toward OFF: an unwired or broken input powers the board down
    // rather than keeping it awake on the battery.
    pinMode(IGN_SENSE_GPIO, IGN_ACTIVE_HIGH ? INPUT_PULLDOWN : INPUT_PULLUP);
    powerLoop();
    Serial.printf("[power] policy=ignition  GPIO%d active-%s  ignition %s  "
                  "(debounce on %ums / off %ums, power-down %lus after off)\n",
                  IGN_SENSE_GPIO, IGN_ACTIVE_HIGH ? "high" : "low",
                  g_ign.on ? "ON" : "OFF", (unsigned)IGN_ON_DEBOUNCE_MS,
                  (unsigned)IGN_OFF_DEBOUNCE_MS,
                  (unsigned long)(IGN_OFF_SLEEP_MS / 1000));
  } else {
    Serial.printf("[power] policy=bus-quiet  files close after %ums of bus "
                  "silence, power-down after %lus\n",
                  (unsigned)CAN_BUS_IDLE_CLOSE_MS,
                  (unsigned long)(CAN_MAX_AWAKE_MS / 1000));
  }
}

void powerLoop() {
  if (kPolicy != POWER_IGNITION) return;
  const bool first = !g_ign.init;
  const bool was = g_ign.on;
  const bool on = ignDebounce(g_ign, readIgnRaw(), millis(),
                              IGN_ON_DEBOUNCE_MS, IGN_OFF_DEBOUNCE_MS);
  if (!first && on != was)
    Serial.printf("[power] ignition %s\n", on ? "ON" : "OFF");
}

PowerPolicy powerPolicy() { return kPolicy; }

PowerThresholds powerThresholdsNow() {
  return powerThresholds(kPolicy, CAN_BUS_IDLE_CLOSE_MS, CAN_MAX_AWAKE_MS,
                         IGN_OFF_DEBOUNCE_MS, IGN_OFF_SLEEP_MS);
}

uint32_t powerQuietNowMs() {
  return powerQuietMs(kPolicy, filestoreBusQuietMs(), g_ign, millis());
}

bool powerMayRecordNow() {
  const PowerThresholds t = powerThresholdsNow();
  return powerMayRecord(kPolicy, powerQuietNowMs(), t);
}

bool powerIgnitionOn() { return kPolicy != POWER_IGNITION || g_ign.on; }

#pragma once

// Power-off policy, runtime half: reads the ignition input and answers "how
// long has the trip looked over" for the configured POWER_POLICY. The pure
// logic (policy clock, debounce, thresholds) is powerpolicy.h.

#include <stdint.h>

#include "powerpolicy.h"

// Configure the ignition input (ignition policy only) and take its first
// sample. Call once from setup(), BEFORE the boot-time sleep check.
void powerBegin();

// Sample + debounce the ignition input. Call every loop() pass.
void powerLoop();

PowerPolicy     powerPolicy();
PowerThresholds powerThresholdsNow();

// The policy's clock: bus quiet (bus-quiet) or ignition-off time (ignition).
uint32_t powerQuietNowMs();

// Whether the filestore may open a file now (powerMayRecord, powerpolicy.h).
bool powerMayRecordNow();

// Debounced ignition level. Always true under bus-quiet.
bool powerIgnitionOn();

// Why this boot happened (powerpolicy.h). WAKE_NA under bus-quiet.
WakeSource powerWakeSource();

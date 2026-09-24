#pragma once

// ---------------------------------------------------------------------------
// Transceiver sleep control -- the ONE place that may cut this board's power.
//
// The TCAN1043's INH pin gates the buck. Per its datasheet INH does not drop
// on its own in normal mode: sleep is MCU-commanded (EN high + nSTB low) and
// INH follows ~20-50 us later. So this function is not reacting to a power
// cut, it IS the power cut, and it is the only thing that has to respect the
// go-to-sleep invariant in sleepguard.h.
//
// 🔴 NOT WIRED ON THE ESP32-CAN-X2. The dev board has no INH path and no
// EN/nSTB control -- those arrive with the carrier board. Until then this
// evaluates the guard, logs the verdict, and returns; it cannot actually
// remove power. That is deliberately still useful: it is the exact instant the
// bench power-cut test cuts the rail, so the test measures the real ordering
// rather than a convenient one.
//
// ⚠️ The as-built carrier part is `TCAN1043GDRQ1` (non-H), not the
// TCAN1043A-Q1 whose timings are quoted above. See config.h.
// ---------------------------------------------------------------------------

#include <stdint.h>

#include "sleepguard.h"

// Evaluate the invariant and, if it holds, command the transceiver to sleep.
//
// Returns the verdict. Anything other than SLEEP_OK means nothing was
// commanded and the board stays powered -- refusing is always safe, because
// the cost of a refusal is that the logger keeps running.
SleepVerdict transceiverRequestSleep();

// Whether the hardware can actually cut power on this build/board. False on
// the ESP32-CAN-X2, so a caller can tell "sleep succeeded" from "sleep was
// allowed but there is nothing here to switch".
bool transceiverHasInhPath();

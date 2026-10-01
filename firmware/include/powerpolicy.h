#pragma once

// ---------------------------------------------------------------------------
// WHAT ENDS A TRIP: the power-off policy, as pure functions.
//
//   POWER_BUS_QUIET  (default) files close and power-down follow bus silence.
//                    Right for a vehicle whose diagnostic bus talks whenever
//                    the vehicle is on.
//   POWER_IGNITION   files close and power-down follow a separate, debounced
//                    ignition input. NEVER bus silence alone: some vehicles
//                    keep the diagnostic bus silent while running unless a
//                    tester polls it, so on those "quiet" is not "off".
//
// Both policies feed the SAME guard (sleepguard.h). The policy only picks the
// clock -- "how long has the trip looked over" -- so the invariant (never
// sleep on an open file; close-then-sleep at the backstop) is written once.
//
// Pure and header-only, like sleepguard.h, so test/test_powerpolicy can drive
// it natively. GPIO reads live in power.cpp.
// ---------------------------------------------------------------------------

#include <stdint.h>

enum PowerPolicy : uint8_t {
  POWER_BUS_QUIET = 0,
  POWER_IGNITION  = 1,
};

static inline const char *powerPolicyName(PowerPolicy p) {
  return p == POWER_IGNITION ? "ignition" : "bus-quiet";
}

// ---------------------------------------------------------------------------
// Ignition input debounce.
//
// Asymmetric on purpose: ON is believed quickly, OFF slowly, because the cost
// of a false OFF (closing files and cutting power mid-trip, e.g. on a supply
// dip while cranking) is far higher than the cost of a late OFF.
//
// `sinceMs` is when the input actually changed (the start of the run that got
// confirmed), not when the debounce confirmed it. So time-off is measured from
// the real edge, and is already >= offMs at the instant OFF is believed.
// ---------------------------------------------------------------------------
struct IgnDebounce {
  bool     init;      // false until the first sample
  bool     on;        // debounced level
  bool     pending;   // raw differs from `on`, waiting out the window
  uint32_t edgeMs;    // when the pending run started
  uint32_t sinceMs;   // when `on` last changed (the real edge)
};

// Feed one raw sample. Returns the debounced level. The first sample is taken
// as-is: a board that boots with ignition off is off from boot, which is what
// lets the boot-time check (sleepguard.h layer 2) power it back down.
static inline bool ignDebounce(IgnDebounce &d, bool raw, uint32_t now,
                               uint32_t onMs, uint32_t offMs) {
  if (!d.init) {
    d.init = true;
    d.on = raw;
    d.pending = false;
    d.sinceMs = now;
    return d.on;
  }
  if (raw == d.on) {          // a glitch shorter than the window is forgotten
    d.pending = false;
    return d.on;
  }
  if (!d.pending) {
    d.pending = true;
    d.edgeMs = now;
  }
  const uint32_t need = raw ? onMs : offMs;
  if ((int32_t)(now - d.edgeMs) >= (int32_t)need) {
    d.on = raw;
    d.pending = false;
    d.sinceMs = d.edgeMs;
  }
  return d.on;
}

// Milliseconds the ignition has been (debounced) OFF. 0 while on, and 0
// before the first sample: an input never read is not evidence of anything.
static inline uint32_t ignOffForMs(const IgnDebounce &d, uint32_t now) {
  if (!d.init || d.on) return 0;
  const int32_t t = (int32_t)(now - d.sinceMs);
  return t > 0 ? (uint32_t)t : 0;
}

// ---------------------------------------------------------------------------
// The policy's clock and thresholds.
// ---------------------------------------------------------------------------

// "How long has the trip looked over", in the policy's terms. This is what
// the sleep guard and the file-close test compare against their thresholds.
//
// Under POWER_IGNITION the bus is NOT consulted at all: an ignition that is on
// holds this at 0 however long the bus has been silent.
static inline uint32_t powerQuietMs(PowerPolicy p, uint32_t busQuietMs,
                                    const IgnDebounce &ign, uint32_t now) {
  return p == POWER_IGNITION ? ignOffForMs(ign, now) : busQuietMs;
}

struct PowerThresholds {
  uint32_t closeMs;   // quiet this long: close every file
  uint32_t sleepMs;   // quiet this long: command power-down (close first)
};

// Per-policy thresholds. Under POWER_IGNITION files close the moment OFF is
// believed (closeMs == the OFF debounce, which ignOffForMs already covers).
// A closeMs of 0 would make "on" (quiet 0) look like "off", so it is clamped.
static inline PowerThresholds powerThresholds(PowerPolicy p,
                                              uint32_t busCloseMs,
                                              uint32_t busSleepMs,
                                              uint32_t ignOffMs,
                                              uint32_t ignSleepMs) {
  PowerThresholds t;
  if (p == POWER_IGNITION) {
    t.closeMs = ignOffMs ? ignOffMs : 1;
    t.sleepMs = ignSleepMs;
  } else {
    t.closeMs = busCloseMs;
    t.sleepMs = busSleepMs;
  }
  return t;
}

// Whether the filestore may open (or reopen) a file. Under POWER_IGNITION a
// file must not reopen while the ignition is off, or the next snapshot would
// undo the close that power-down depends on. POWER_BUS_QUIET keeps its
// existing behaviour (bus activity re-arms it in filestore.cpp).
static inline bool powerMayRecord(PowerPolicy p, uint32_t quietMs,
                                  const PowerThresholds &t) {
  return p != POWER_IGNITION || quietMs < t.closeMs;
}

// ---------------------------------------------------------------------------
// THE TX GATE. Every path that can put a bit on the vehicle bus -- an OBD
// request, a SELFTEST frame, and the choice of a TWAI mode that ACKs (NORMAL)
// or drives (NO_ACK) -- asks this one function first (cantx.h).
//
// Under POWER_IGNITION nothing transmits while the ignition is off: a parked
// car's bus is woken by the car, never by the logger. POWER_BUS_QUIET has no
// ignition input, so the gate is open there (behaviour unchanged).
// ---------------------------------------------------------------------------
static inline bool canTxGate(PowerPolicy p, bool ignitionOn) {
  return p != POWER_IGNITION || ignitionOn;
}

// ---------------------------------------------------------------------------
// WAKE SOURCE: why this boot happened, read once in setup().
//
//   ignition  the ignition input reads ON at boot.
//   bus-wake  ignition OFF and no USB host: the carrier's TCAN1043 saw bus
//             activity and raised INH. The board closes nothing (nothing is
//             open yet), transmits nothing, and goes back to sleep after
//             IGN_BUSWAKE_AWAKE_MS unless the ignition comes on.
//   usb-bench ignition OFF with a USB host attached: powered from the bench.
//             Treated exactly like bus-wake (no TX, no recording, go-to-sleep
//             commanded); a USB supply simply keeps the board up anyway.
//   n/a       bus-quiet policy: no ignition input to read.
// ---------------------------------------------------------------------------
enum WakeSource : uint8_t {
  WAKE_NA = 0,
  WAKE_IGNITION,
  WAKE_BUS,
  WAKE_USB_BENCH,
};

static inline WakeSource wakeSourceAtBoot(PowerPolicy p, bool ignitionOn,
                                          bool usbHost) {
  if (p != POWER_IGNITION) return WAKE_NA;
  if (ignitionOn) return WAKE_IGNITION;
  return usbHost ? WAKE_USB_BENCH : WAKE_BUS;
}

static inline const char *wakeSourceName(WakeSource w) {
  switch (w) {
    case WAKE_IGNITION:  return "ignition";
    case WAKE_BUS:       return "bus-wake";
    case WAKE_USB_BENCH: return "usb-bench";
    default:             return "n/a";
  }
}

// How long the ignition may read OFF before go-to-sleep is commanded.
//   * Ignition seen ON this boot: IGN_OFF_SLEEP_MS after the (debounced) OFF.
//   * Never seen ON (bus-wake / usb-bench boot): the short bus-wake window,
//     timed from boot, so a door-unlock wake does not hold the board up.
//   * A watchdog/panic reset with ignition OFF: 0, back to sleep at once --
//     the reset re-evaluates the input instead of trusting the old state.
static inline uint32_t ignSleepAfterMs(bool seenOnThisBoot, bool watchdogBoot,
                                       uint32_t offSleepMs, uint32_t busWakeMs) {
  if (seenOnThisBoot) return offSleepMs;
  return watchdogBoot ? 0 : busWakeMs;
}

// ---------------------------------------------------------------------------
// TCAN1043 MODE PINS on the carrier (EN = IO39, nSTB = IO38, both with 10 k
// pull-downs). TXD/RXD are NOT connected: the TCAN1043 is a wake sensor and
// the INH switch for the board's own supply, never a CAN path.
//
//   mode         EN  nSTB  INH   used for
//   standby       0    0   on    power-up state (the pull-downs)
//   silent        0    1   on    ignition ON: hold-up, receiver only
//   go-to-sleep   1    0   off   power-down: INH drops after tGO_TO_SLEEP
//   normal        1    1   on    NEVER: it is the transmit mode
//
// There is deliberately no XCVR_NORMAL value, so no code path can ask for it.
// ---------------------------------------------------------------------------
enum XcvrMode : uint8_t {
  XCVR_STANDBY = 0,
  XCVR_SILENT,
  XCVR_GO_TO_SLEEP,
};

struct XcvrPins {
  bool en;
  bool nstb;
};

static inline XcvrPins xcvrPinsFor(XcvrMode m) {
  switch (m) {
    case XCVR_SILENT:      return {false, true};
    case XCVR_GO_TO_SLEEP: return {true, false};
    default:               return {false, false};
  }
}

static inline const char *xcvrModeName(XcvrMode m) {
  switch (m) {
    case XCVR_SILENT:      return "silent";
    case XCVR_GO_TO_SLEEP: return "go-to-sleep";
    default:               return "standby";
  }
}

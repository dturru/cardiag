#pragma once

// ---------------------------------------------------------------------------
// CAN controller recovery: a stuck transmitter is brought back without a
// reboot. Pure (no driver calls) so test/test_canrecov drives it; main.cpp
// feeds it the TWAI status once a second and carries out what it returns.
//
// Why: bench 4 (2026-10-05) overnight, SELFTEST on a bare X2. After 1 h 55 min
// a burst of 107 bus errors took the controller to bus-off (txfail climbed at
// the full ~235 frames/s, tec read 128). Nothing called
// twai_initiate_recovery(), so it stayed there for the remaining 9 h with
// Wi-Fi and the hub link fine and no frames at all.
//
// Who ACKs SELFTEST on a bare X2? Nobody needs to: SELFTEST runs the
// controller in TWAI_MODE_NO_ACK, which treats a missing ACK as success, so
// the board's own transceiver loopback is enough (no second node, no
// terminator). That is also why tec=128 there was NOT the no-ACK signature: a
// NO_ACK controller still counts bit/stuff/form errors, and 107 of those in
// one burst is what took it to bus-off. The cause of the burst is unknown
// (bench wiring, USB ground); recovery does not depend on knowing it.
//
//   BUS_OFF          -> RECOVER: twai_initiate_recovery(). The controller
//                       waits for 128 x 11 recessive bits, then STOPPED.
//   STOPPED after a RECOVER, or still BUS_OFF/RECOVERING after
//   CANRECOV_RECOVER_TIMEOUT_MS
//                    -> RESTART: reinstall the driver in the current mode.
//   RUNNING, tec >= 128 (error-passive) and TX failing continuously for
//   CANRECOV_PASSIVE_MS
//                    -> RESTART.
//
// RESTART goes through applyMode(), which picks the TWAI mode from the TX gate
// (cantx.h) every time, so recovery can never put a frame on the bus that the
// gate would refuse: with the gate shut the driver comes back LISTEN_ONLY,
// which cannot go bus-off in the first place. Nothing here transmits.
//
// Actions are spaced by an exponential backoff (CANRECOV_BACKOFF_MIN_MS ..
// _MAX_MS) so a bus that is genuinely broken is not hammered; the backoff
// resets after CANRECOV_STABLE_MS of healthy running.
// ---------------------------------------------------------------------------

#include <stdint.h>

#define CANRECOV_PASSIVE_MS          5000u
#define CANRECOV_RECOVER_TIMEOUT_MS  5000u
#define CANRECOV_BACKOFF_MIN_MS      1000u
#define CANRECOV_BACKOFF_MAX_MS     60000u
#define CANRECOV_STABLE_MS          60000u
#define CANRECOV_TEC_PASSIVE          128u

enum CanBusState : uint8_t {      // twai_state_t, mirrored for native tests
  CANBUS_STOPPED = 0,
  CANBUS_RUNNING,
  CANBUS_BUS_OFF,
  CANBUS_RECOVERING,
};

enum CanRecovAction : uint8_t {
  CANRECOV_NONE = 0,
  CANRECOV_RECOVER,               // twai_initiate_recovery()
  CANRECOV_RESTART,               // reinstall the driver (applyMode)
};

struct CanRecov {
  bool     recovering;            // a RECOVER is in progress
  uint32_t recoverStartMs;
  bool     failing;               // error-passive with TX failures
  uint32_t failingSinceMs;
  uint32_t lastTxFails;
  uint32_t healthySinceMs;
  uint32_t backoffMs;
  uint32_t nextAllowedMs;
  uint32_t recoveries;            // RECOVER + RESTART actions taken
  uint8_t  lastAction;
  uint32_t lastActionMs;
};

static inline void canRecovInit(CanRecov *r, uint32_t now) {
  *r = CanRecov{};
  r->backoffMs = CANRECOV_BACKOFF_MIN_MS;
  r->healthySinceMs = now;
}

static inline const char *canBusStateName(uint8_t s) {
  switch (s) {
    case CANBUS_STOPPED:    return "stopped";
    case CANBUS_RUNNING:    return "running";
    case CANBUS_BUS_OFF:    return "bus_off";
    case CANBUS_RECOVERING: return "recovering";
    default:                return "?";
  }
}

static inline CanRecovAction canRecovAct(CanRecov *r, CanRecovAction a,
                                         uint32_t now) {
  r->recoveries++;
  r->lastAction = a;
  r->lastActionMs = now;
  r->nextAllowedMs = now + r->backoffMs;
  r->backoffMs = (r->backoffMs >= CANRECOV_BACKOFF_MAX_MS / 2)
                     ? CANRECOV_BACKOFF_MAX_MS : r->backoffMs * 2;
  r->failing = false;
  r->recovering = (a == CANRECOV_RECOVER);
  if (r->recovering) r->recoverStartMs = now;
  return a;
}

// One step. `txFails` is a running count of refused transmits (any mode);
// only its growth matters. Call it about once a second while the driver is up.
static inline CanRecovAction canRecovStep(CanRecov *r, uint8_t state,
                                          uint32_t tec, uint32_t txFails,
                                          uint32_t now) {
  const bool newFails = txFails != r->lastTxFails;
  r->lastTxFails = txFails;
  const bool mayAct = (int32_t)(now - r->nextAllowedMs) >= 0;

  if (r->recovering) {
    if (state == CANBUS_STOPPED) {           // recovery finished: start again
      r->recovering = false;
      return canRecovAct(r, CANRECOV_RESTART, now);
    }
    if (now - r->recoverStartMs >= CANRECOV_RECOVER_TIMEOUT_MS) {
      r->recovering = false;                 // the bus never went recessive
      return canRecovAct(r, CANRECOV_RESTART, now);
    }
    return CANRECOV_NONE;
  }

  if (state == CANBUS_BUS_OFF) {
    r->healthySinceMs = now;
    return mayAct ? canRecovAct(r, CANRECOV_RECOVER, now) : CANRECOV_NONE;
  }

  if (state == CANBUS_RUNNING && tec >= CANRECOV_TEC_PASSIVE && newFails) {
    if (!r->failing) {
      r->failing = true;
      r->failingSinceMs = now;
    }
    r->healthySinceMs = now;
    if (now - r->failingSinceMs >= CANRECOV_PASSIVE_MS && mayAct) {
      return canRecovAct(r, CANRECOV_RESTART, now);
    }
    return CANRECOV_NONE;
  }

  // Healthy (or merely error-passive with nothing failing).
  r->failing = false;
  if (state == CANBUS_RUNNING && tec < CANRECOV_TEC_PASSIVE && !newFails) {
    if (now - r->healthySinceMs >= CANRECOV_STABLE_MS) {
      r->backoffMs = CANRECOV_BACKOFF_MIN_MS;
    }
  } else {
    r->healthySinceMs = now;
  }
  return CANRECOV_NONE;
}

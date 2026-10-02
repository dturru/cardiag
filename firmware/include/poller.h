#pragma once

// MODE_POLL runtime: runs the hub-supplied poll plan (pollplan.h) without
// blocking. canTask keeps receiving every frame and hands OBD replies
// (0x7E8..0x7EF) to a queue; loop() matches them, retires timeouts and sends
// the next due request -- one in flight, every transmit through the TX gate
// (cantx.h). Nothing here waits on the bus.
//
// The plan lives in NVS (namespace "pollplan"), which an app upload does not
// touch, so it survives reboot and reflash.

#include <stddef.h>
#include <stdint.h>
#include <driver/twai.h>

#include "pollplan.h"

// Load the stored plan and create the reply queue. Call once from setup(),
// before canTask can run in MODE_POLL.
void pollerBegin();

// Entering / leaving MODE_POLL. Enter restarts the schedule and says what it
// will poll -- or that there is no plan and the mode idles.
void pollerEnter();
void pollerExit();

// loop(), MODE_POLL only. Non-blocking.
void pollerTick();

// canTask, MODE_POLL only: offer a received frame. Cheap; never blocks.
void pollerNoteFrame(const twai_message_t &rx);

// POST /api/v1/pollplan. Validates, persists, applies. On POLLPLAN_OK the
// canonical hash is written to hashOut. Empty entries clears the plan.
PollPlanResult pollerSetFromJson(const char *json, size_t n,
                                 char hashOut[POLL_HASH_HEX + 1]);

// For /api/v1/session: false if no plan is stored.
bool    pollerHasPlan();
// False if NVS was unreadable (or held an invalid plan): session hash = null.
bool    pollerNvsOk();
uint8_t pollerEntries();
void    pollerHash(char out[POLL_HASH_HEX + 1]);

// Serial status line (MODE_POLL stats interval).
void pollerPrintStats();

// True once any plan entry has been answered since the last pollerEnter().
bool pollerAnyReply();

uint32_t pollerRxDropped();   // replies lost to a full queue since boot

#pragma once

// ---------------------------------------------------------------------------
// Board: Autosport Labs ESP32-CAN-X2  (ESP32-S3-WROOM-1-N8R8)
// https://wiki.autosportlabs.com/ESP32-CAN-X2
//
// These pins come from the vendor wiki. The vendor's own Arduino examples use
// CAN1_TX / CAN1_RX macros supplied by their board variant — those do not exist
// under PlatformIO with board = esp32-s3-devkitc-1, so we define them here.
// ---------------------------------------------------------------------------

#define CAN1_TX_GPIO 7   // TWAI TX  (built-in controller)
#define CAN1_RX_GPIO 6   // TWAI RX

// CAN2 is an MCP2515 on SPI (CS 10, SCK 12, MISO 13, MOSI 11, INT 3).
// Unused in Phase 0 — see firmware/README.md for why.

// ---------------------------------------------------------------------------
// Phase 0 mode select. Exactly one.
//
//   MODE_SELFTEST — TWAI internal loopback. No wiring, nothing connected,
//                   not plugged into anything. Proves toolchain, driver
//                   install, timing config, and frame handling in isolation.
//
//   MODE_LISTEN   — Listen-only sniffer. The controller never transmits and
//                   never even emits ACK bits, so it is provably passive.
//                   This is the ONLY mode that should touch the car first.
// ---------------------------------------------------------------------------

//   MODE_POLL     - OBD-II polling (Phase 1a). ***THIS MODE TRANSMITS.***
//                   Sends standard OBD-II Mode 01 requests and decodes the
//                   replies. Only run it AFTER SELFTEST and LISTEN have both
//                   passed -- that sequencing is the entire justification for
//                   the passive modes existing.

#define MODE_SELFTEST 0
#define MODE_LISTEN   1
#define MODE_POLL     2

#ifndef CARDIAG_MODE
#define CARDIAG_MODE MODE_SELFTEST
#endif

// 2012 Civic HS-CAN. Standard for essentially all modern passenger vehicles.
#define CAN_BITRATE_500K 1

// How often to print the rolling statistics line, milliseconds.
#define STATS_INTERVAL_MS 1000

// ---------------------------------------------------------------------------
// MODE_POLL tuning. Unused by the Phase 0 modes.
// ---------------------------------------------------------------------------

// How long to wait for an ECU to answer one request. ECU response latency is
// typically 10-30 ms; 100 ms is deliberately generous for first contact and
// should be tightened once real numbers exist.
#define OBD_RESPONSE_TIMEOUT_MS 100

// Gap between consecutive requests. Requests are strictly serialized -- send,
// wait for the reply or time out, then send the next. Never flood the bus.
#define OBD_INTER_REQUEST_MS 5

// How often to run a full sweep of the supported PID list, milliseconds.
#define OBD_POLL_INTERVAL_MS 1000

// PIDs packed into a single Mode 01 request. The standard permits up to 6, and
// that is the difference between ~30 samples/s and ~180 samples/s -- the number
// that sets this project's sampling ceiling.
//
// LEAVE THIS AT 1 until the 20-minute bench test says otherwise: not every ECU
// honours multi-PID requests, and a rejected request looks like a dead bus.
// Record the answer in docs/hardware.md when it is known.
#define OBD_MULTI_PID_PER_REQUEST 1

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

#define MODE_SELFTEST 0
#define MODE_LISTEN   1

#ifndef CARDIAG_MODE
#define CARDIAG_MODE MODE_SELFTEST
#endif

// 2012 Civic HS-CAN. Standard for essentially all modern passenger vehicles.
#define CAN_BITRATE_500K 1

// How often to print the rolling statistics line, milliseconds.
#define STATS_INTERVAL_MS 1000

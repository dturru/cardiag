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

//   MODE_SNIFF    - Per-ID sniffer table (listen-only, passive). One row per
//                   CAN ID with a baseline and sticky change marks. This is
//                   the mode for answering "which byte is the brake pedal".

#define MODE_SELFTEST 0
#define MODE_LISTEN   1
#define MODE_POLL     2
#define MODE_SNIFF    3

// The mode the board boots into when NVS holds nothing usable. Modes are now
// switched at RUNTIME (serial keys or the BOOT button) and the choice is
// persisted, so this is a default and not the mode -- changing it no longer
// requires a reflash.
#ifndef CARDIAG_MODE
#define CARDIAG_MODE MODE_SNIFF
#endif

// ---------------------------------------------------------------------------
// Sniffer table
// ---------------------------------------------------------------------------

// The Civic showed 41 distinct IDs. 64 leaves headroom without pretending the
// table is unbounded; anything past it increments a visible overflow counter.
#define SNIFF_MAX_IDS 64

// Twice a second is fast enough to feel live and slow enough to stay cheap:
// ~41 rows at ~55 chars is ~2.3 KB per block, so ~5 KB/s against the ~40 KB/s
// the raw frame dump produced. Roughly an 8x cut, which is the whole reason
// this mode exists -- USB CDC drops rather than blocks under load.
#define SNIFF_PRINT_INTERVAL_MS 500

// Frames of evidence required before calling a byte a heartbeat. Below this
// every byte looks volatile and the table would suppress real signals.
#define SNIFF_MIN_SAMPLES 20

// A byte changing on at least this percent of consecutive frames is a rolling
// counter or checksum, not data. Verified against the 2026-09-07 Civic capture:
// counter nibbles move every single frame, real signals move far less often.
#define SNIFF_HEARTBEAT_PCT 90

// ---------------------------------------------------------------------------
// Laptop-free controls
// ---------------------------------------------------------------------------

// Vendor wiki: orange LED is power (fixed), blue is a user LED on GPIO2.
#define PIN_USER_LED     2

// The BOOT button. Free to reuse once the board is running.
#define PIN_MODE_BUTTON  0

#define BUTTON_DEBOUNCE_MS 50

// MODE_POLL TRANSMITS, so it is deliberately unreachable by the button and is
// never restored from NVS at boot. Entering it always takes a live, confirmed
// keystroke -- a stray press must never start transmitting onto a vehicle bus.
#define POLL_CONFIRM_WINDOW_MS 5000

// ---------------------------------------------------------------------------
// Wi-Fi access point + web UI
// ---------------------------------------------------------------------------

// The board hosts its own AP; there is no network in a car to join. Connect a
// phone to this SSID and open http://192.168.4.1/.
#define WIFI_AP_SSID    "cardiag"

// WPA2 needs 8+ characters. An OPEN AP would let anyone in range read the car's
// bus, so it is not open. The real password lives in the gitignored secrets.h
// (see secrets.h.example) -- this repo is public and a credential in history
// outlives the commit that removed it. The fallback keeps a fresh clone
// building.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_AP_PASS
#define WIFI_AP_PASS "cardiag-default"
#endif
#define WIFI_AP_CHANNEL 6

// The page polls a small JSON snapshot instead of streaming frames. Streaming
// 1000 fps over Wi-Fi is exactly how you turn a sniffer into a frame-dropper.
#define WEB_POLL_HINT_MS 300

// Snapshot buffer. 64 rows of ~90 chars plus the header, with headroom.
#define WEB_JSON_CAP 12288

// ---------------------------------------------------------------------------
// CAN receive task
// ---------------------------------------------------------------------------

// The Wi-Fi stack lives on core 0. Draining CAN from loop() would put the
// sniffer behind the web server's blocking client handling, so the drain gets
// its own task at a priority above loopTask (which runs at 1). It BLOCKS on
// twai_receive rather than polling, so it costs nothing while the bus is idle.
#define CAN_TASK_CORE   1
#define CAN_TASK_PRIO   5
#define CAN_TASK_STACK  4096
#define CAN_RX_WAIT_MS  100

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

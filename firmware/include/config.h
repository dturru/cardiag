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
// ---------------------------------------------------------------------------
// Hub integration (protocol v1). See docs/hub-integration-plan.md.
// ---------------------------------------------------------------------------

// The hub network the logger joins as a station. Credentials live in the
// gitignored secrets.h; these are only the fallbacks so a fresh checkout still
// compiles. STA is tried first; on failure the board falls back to its own AP.
#ifndef WIFI_STA_SSID
#define WIFI_STA_SSID "carhub"
#endif
#ifndef WIFI_STA_PASS
#define WIFI_STA_PASS "carhub-default"
#endif
// How long to wait for the hub network before falling back to AP mode.
#define WIFI_STA_TIMEOUT_MS 8000
// Retry the hub network periodically, so the logger joins when the car gets home.
#define WIFI_STA_RETRY_MS   60000
// First retry after an UNEXPECTED drop. A dropped link may be a blip rather
// than a hub that went home, and 60 s of AP for a blip is a bad trade -- but
// retrying every 10 s forever would tear the AP down repeatedly for a hub that
// is genuinely absent, so this is a one-shot and the cadence then returns to
// WIFI_STA_RETRY_MS. See hublink.cpp.
#define WIFI_STA_QUICK_RETRY_MS 10000

// Shared token for the mutating /api/v1 endpoints. Protocol v1 2.4.
// Override in secrets.h. A default this obvious is intentional: it should look
// wrong in a packet capture if it was never changed.
#ifndef HUB_API_TOKEN
#define HUB_API_TOKEN "change-me"
#endif

#define HUB_UDP_PORT        5005
#define HUB_SNAPSHOT_HZ     5      // protocol 1.5 default; 1-10
#define HUB_FAST_HZ         20     // fast-ID list ceiling
#define HUB_FAST_MAX_IDS    4

// LittleFS partition (partitions_cardiag_8mb.csv) less ~8% LittleFS overhead.
// Used to PROJECT snapshot-log retention. Phase B reports the real figure from
// LittleFS.totalBytes() alongside it; when the two disagree, believe LittleFS.
#define HUB_FS_USABLE_BYTES 3738173ull

// ---------------------------------------------------------------------------
// Filestore (protocol v1 section 2). See filestore.h for the tier argument.
// ---------------------------------------------------------------------------

#define FS_DIR "/log"

// Rotation size. Small files are better here for three reasons that all
// outrank the per-file overhead: retention deletes a whole file at a time so
// granularity is the quantum of data loss; an interrupted sync re-fetches at
// most this much; and the hub's watermark advances this often. 64 KB is ~7 s
// of Tier C at the measured 9,450 B/s and ~5.7 min of Tier B at 14 ids.
#define FS_FILE_MAX_BYTES 65536u

// Tier A (frame-level: the change log, and the raw ring once SD exists) may
// never occupy more than this share of the partition. Ordering alone (delete A
// before B) is not enough: Tier A outruns Tier B fifty to one, so without a cap
// it would fill the disk between two snapshot blocks and retention would spend
// its whole life deleting. The cap is what makes the Tier B retention
// projection on /api/v1/session mean anything.
//
// ⚠ Was FS_TIER_C_MAX_PCT. The change log is Tier A -- deduplicated raw frames
// -- not Tier C, which is the trip bookends. See filestore.h.
#define FS_TIER_A_MAX_PCT 40

// Report usage on /api/v1/session well before retention has to delete
// anything. Protocol 2.3: deletion is never the first the hub hears of it.
#define FS_WARN_USAGE_PCT 70

// One snapshot block per second. This is the sampling rate the whole storage
// budget is built on -- changing it changes the retention projection.
#define FS_SNAPSHOT_PERIOD_MS 1000

// Bytes written to flash per filestore pass. LittleFS writes block-erase, and
// a long write in loop() is a long time not serving HTTP or streaming UDP.
// Draining is spread across passes instead; at ~1 kHz this ceiling is far
// above the 9.5 KB/s the change log produces.
#define FS_FLUSH_BUDGET_BYTES 2048u

// Index-table ceiling. At FS_FILE_MAX_BYTES this covers the whole partition
// with room to spare; files past it are on disk but not listed, which the
// stats report rather than hide.
#define FS_MAX_FILES 96

// Compact the change log once this share of it has been written to flash.
// Compaction memmoves the tail down, so it wants to be rare and bulk.
#define FS_COMPACT_AT_PCT 25

// Observed distinct CAN ids on the 2012 Civic. UNCONFIRMED -- the only measured
// figure is 14, from a changes-only stimulus capture. 41 is the working number
// pending a 60 s MODE_LISTEN capture (next Texas trip).
// NOTHING IS SIZED FROM THIS: the logger counts ids at runtime and reports the
// projection on /api/v1/session and hub/health. SNIFF_MAX_IDS (64) is the only
// hard bound, and it also covers the BMW, which will differ.
#define CIVIC_OBSERVED_IDS_UNCONFIRMED 41

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

// ---------------------------------------------------------------------------
// Recorder buffers
// ---------------------------------------------------------------------------

// A record is 16 bytes. 4 MB of raw ring is ~262k frames, roughly four minutes
// at the ~1000 fps a Civic idles at -- enough history to keep the interesting
// thirty seconds after the fact.
#define REC_RAW_BYTES_PSRAM (4u * 1024u * 1024u)
#define REC_CHG_BYTES_PSRAM (2u * 1024u * 1024u)

// Without PSRAM the feature shrinks rather than disappears, so a board that
// fails to bring the octal RAM up is still usable and says so.
#define REC_RAW_BYTES_FALLBACK (64u * 1024u)
#define REC_CHG_BYTES_FALLBACK (32u * 1024u)

// Chunk size for streaming a CSV download out of the web server.
#define REC_CSV_CHUNK 2048

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

// cardiag — Phase 0 bring-up
//
// Two modes, selected in include/config.h:
//   MODE_SELFTEST : TWAI internal loopback. Nothing connected. Proves the
//                   toolchain, driver, timing config, and frame handling.
//   MODE_LISTEN   : Listen-only sniffer. Never transmits, never ACKs.
//                   Safe first contact with a live vehicle bus.
//
// API verified 2026-08-20 against ESP-IDF v5.x legacy TWAI driver docs and the
// Autosport Labs reference example. Note the ESP-IDF v6 handle-based API
// (esp_twai.h / twai_new_node_onchip) is a DIFFERENT driver — not this one.

#include <Arduino.h>
#include "driver/twai.h"
#include "config.h"

// ---------------------------------------------------------------------------
// Rolling stats. On a live bus the useful first question is not "what does
// this byte mean" but "how busy is this bus and how many distinct messages
// are on it." That tells you immediately whether you're actually connected.
// ---------------------------------------------------------------------------

static uint32_t g_frames         = 0;   // frames since boot
static uint32_t g_framesLastTick = 0;   // for frames/sec
static uint32_t g_lastStatsMs    = 0;

// Distinct 11-bit standard IDs seen, as a bitmap. 2048 bits = 256 bytes.
static uint8_t  g_seenStd[256]   = {0};
static uint16_t g_uniqueIds      = 0;

static void noteId(uint32_t id, bool extended) {
  if (extended || id >= 2048) return;   // Phase 0 tracks standard IDs only
  const uint16_t byteIdx = id >> 3;
  const uint8_t  bitMask = 1 << (id & 0x7);
  if (!(g_seenStd[byteIdx] & bitMask)) {
    g_seenStd[byteIdx] |= bitMask;
    g_uniqueIds++;
  }
}

static void printFrame(const twai_message_t &msg) {
  Serial.printf("[%8lu] %s 0x%03lX  dlc=%u  ",
                (unsigned long)millis(),
                msg.extd ? "EXT" : "STD",
                (unsigned long)msg.identifier,
                msg.data_length_code);

  if (msg.rtr) {
    Serial.print("RTR");
  } else {
    for (int i = 0; i < msg.data_length_code; i++) {
      Serial.printf("%02X ", msg.data[i]);
    }
  }
  Serial.println();
}

// ---------------------------------------------------------------------------
// Driver bring-up
// ---------------------------------------------------------------------------

static bool startTwai(twai_mode_t mode) {
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
      (gpio_num_t)CAN1_TX_GPIO, (gpio_num_t)CAN1_RX_GPIO, mode);

  // The default RX queue is shallow. Deepening it buys headroom on a busy bus,
  // but it is NOT the real fix — Phase 2 drains in an ISR into a ring buffer.
  // Until then, a full queue shows up as rx_missed in the stats line, which is
  // exactly the signal we want to see rather than silently losing frames.
  g.rx_queue_len = 32;

  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t err = twai_driver_install(&g, &t, &f);
  if (err != ESP_OK) {
    Serial.printf("FATAL: twai_driver_install failed: %s\n", esp_err_to_name(err));
    return false;
  }

  err = twai_start();
  if (err != ESP_OK) {
    Serial.printf("FATAL: twai_start failed: %s\n", esp_err_to_name(err));
    return false;
  }

  return true;
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(2000);   // let USB CDC enumerate before the first print

  Serial.println();
  Serial.println("cardiag — Phase 0");

#if CARDIAG_MODE == MODE_SELFTEST
  Serial.println("Mode: SELF-TEST (internal loopback, nothing need be connected)");
  // NO_ACK lets the controller transmit without another node acknowledging.
  // Combined with message.self below, the frame comes straight back to us.
  if (!startTwai(TWAI_MODE_NO_ACK)) { while (true) delay(1000); }

#elif CARDIAG_MODE == MODE_LISTEN
  Serial.println("Mode: LISTEN-ONLY (passive — never transmits, never ACKs)");
  Serial.println("Bitrate: 500 kbit/s");
  if (!startTwai(TWAI_MODE_LISTEN_ONLY)) { while (true) delay(1000); }

#else
  #error "CARDIAG_MODE must be MODE_SELFTEST or MODE_LISTEN"
#endif

  Serial.println("TWAI started.");
  Serial.println();
  g_lastStatsMs = millis();
}

// ---------------------------------------------------------------------------

#if CARDIAG_MODE == MODE_SELFTEST

// Transmit a frame to ourselves once per second and verify it comes back.
// If TX succeeds but nothing is received, the driver is running but the
// self-reception request is not being honoured — a config problem, not wiring.
static void selfTestTick() {
  static uint32_t seq = 0;

  twai_message_t tx = {};       // zero-init: clears extd/rtr/ss/dlc_non_comp
  tx.identifier       = 0x100;
  tx.self             = 1;      // self-reception request
  tx.data_length_code = 4;
  tx.data[0] = (uint8_t)(seq >> 24);
  tx.data[1] = (uint8_t)(seq >> 16);
  tx.data[2] = (uint8_t)(seq >> 8);
  tx.data[3] = (uint8_t)(seq);
  seq++;

  esp_err_t err = twai_transmit(&tx, pdMS_TO_TICKS(100));
  if (err != ESP_OK) {
    Serial.printf("TX failed: %s\n", esp_err_to_name(err));
  }
}

#endif

void loop() {
  // Drain everything currently queued. Zero timeout — never block the loop.
  twai_message_t rx;
  while (twai_receive(&rx, 0) == ESP_OK) {
    g_frames++;
    noteId(rx.identifier, rx.extd);
    printFrame(rx);
  }

#if CARDIAG_MODE == MODE_SELFTEST
  static uint32_t lastTx = 0;
  if (millis() - lastTx >= 1000) {
    lastTx = millis();
    selfTestTick();
  }
#endif

  const uint32_t now = millis();
  if (now - g_lastStatsMs >= STATS_INTERVAL_MS) {
    const uint32_t elapsed = now - g_lastStatsMs;
    const uint32_t fps     = (g_frames - g_framesLastTick) * 1000UL / elapsed;

    twai_status_info_t st;
    twai_get_status_info(&st);

    Serial.printf(
        "-- %lu fps | %lu total | %u unique IDs | rx_q=%lu missed=%lu "
        "overrun=%lu bus_err=%lu tx_err=%lu\n",
        (unsigned long)fps,
        (unsigned long)g_frames,
        g_uniqueIds,
        (unsigned long)st.msgs_to_rx,
        (unsigned long)st.rx_missed_count,
        (unsigned long)st.rx_overrun_count,
        (unsigned long)st.bus_error_count,
        (unsigned long)st.tx_error_counter);

    g_framesLastTick = g_frames;
    g_lastStatsMs    = now;
  }

  delay(1);
}

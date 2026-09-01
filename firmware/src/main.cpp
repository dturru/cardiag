// cardiag — Phase 0 bring-up
//
// Two modes, selected in include/config.h:
//   MODE_SELFTEST : TWAI internal loopback. Nothing connected. Proves the
//                   toolchain, driver, timing config, and frame handling.
//   MODE_LISTEN   : Listen-only sniffer. Never transmits, never ACKs.
//                   Safe first contact with a live vehicle bus.
//   MODE_POLL     : OBD-II Mode 01 polling (Phase 1a). *** TRANSMITS ***.
//                   Run only after SELFTEST and LISTEN have both passed.
//
// API verified 2026-08-20 against ESP-IDF v5.x legacy TWAI driver docs and the
// Autosport Labs reference example. Note the ESP-IDF v6 handle-based API
// (esp_twai.h / twai_new_node_onchip) is a DIFFERENT driver — not this one.

#include <Arduino.h>
#include "driver/twai.h"
#include "config.h"

#if CARDIAG_MODE == MODE_POLL
#include "obd.h"
#endif

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

#if CARDIAG_MODE == MODE_POLL
static uint32_t      g_supported[OBD_BITMAP_WORDS] = {0};
static const ObdPid *g_pollList[16]                = {0};
static uint8_t       g_pollCount                   = 0;
#endif

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

#elif CARDIAG_MODE == MODE_POLL
  Serial.println("Mode: OBD-II POLL (Phase 1a)");
  Serial.println("*** THIS MODE TRANSMITS. Bench or parked only. ***");
  Serial.println("Bitrate: 500 kbit/s");
  if (!startTwai(TWAI_MODE_NORMAL)) { while (true) delay(1000); }

#else
  #error "CARDIAG_MODE must be MODE_SELFTEST, MODE_LISTEN, or MODE_POLL"
#endif

  Serial.println("TWAI started.");
  Serial.println();

#if CARDIAG_MODE == MODE_POLL
  // Ask the car what it actually supports before asking for anything. A PID
  // that is absent here will never answer, and polling it just spends the
  // request budget on timeouts.
  const uint8_t n = obdDiscoverSupported(g_supported);
  Serial.printf("supported PIDs (0x01-0x60): %u\n", n);

  if (n == 0) {
    Serial.println("WARNING: discovery returned nothing.");
    Serial.println("  Engine off / key not in accessory, wiring, or the bus is asleep.");
    Serial.println("  MODE_LISTEN should show traffic before this mode can work.");
  }

  Serial.print("polling:");
  g_pollCount = 0;
  for (size_t i = 0; i < OBD_PID_TABLE_LEN; i++) {
    if (obdPidSupported(g_supported, OBD_PID_TABLE[i].pid)) {
      g_pollList[g_pollCount++] = &OBD_PID_TABLE[i];
      Serial.printf(" %s", OBD_PID_TABLE[i].name);
    }
  }
  Serial.println();

  // Named so the gap is visible rather than silently absent from the data.
  for (size_t i = 0; i < OBD_PID_TABLE_LEN; i++) {
    if (!obdPidSupported(g_supported, OBD_PID_TABLE[i].pid)) {
      Serial.printf("  (unsupported on this car: %s / PID 0x%02X)\n",
                    OBD_PID_TABLE[i].name, OBD_PID_TABLE[i].pid);
    }
  }
  Serial.println();
#endif

  g_lastStatsMs = millis();
}

// ---------------------------------------------------------------------------

#if CARDIAG_MODE == MODE_POLL

// One sweep of the supported PID list, printed as a single line.
//
// Requests are strictly serialized -- send, wait, then send the next. That is
// slower than pipelining and it is the right default: a flooded bus is a way
// to annoy a real ECU, and the sweep rate is nowhere near the limit anyway.
static void pollTick() {
  Serial.printf("[%8lu]", (unsigned long)millis());

  for (uint8_t i = 0; i < g_pollCount; i++) {
    const ObdPid *p = g_pollList[i];
    ObdResult r;

    if (obdRequest(OBD_MODE_CURRENT_DATA, p->pid, &r) && r.len >= p->nbytes) {
      // NOTE: %f needs full newlib formatting. Arduino-ESP32 ships with it
      // enabled; if these ever print as garbage that is the reason, not the
      // decode maths.
      Serial.printf("  %s %.1f%s", p->name, p->decode(r.data), p->unit);
    } else if (r.multiframe) {
      Serial.printf("  %s MULTIFRAME", p->name);
    } else {
      Serial.printf("  %s --", p->name);
    }

    delay(OBD_INTER_REQUEST_MS);
  }
  Serial.println();
}

#endif

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
#if CARDIAG_MODE == MODE_POLL
  // MODE_POLL must NOT drain the RX queue here: obdRequest() is waiting on
  // exactly those frames, and a second reader silently eats the replies.
  static uint32_t lastPoll = 0;
  if (millis() - lastPoll >= OBD_POLL_INTERVAL_MS) {
    lastPoll = millis();
    pollTick();
  }
#else
  // Drain everything currently queued. Zero timeout — never block the loop.
  twai_message_t rx;
  while (twai_receive(&rx, 0) == ESP_OK) {
    g_frames++;
    noteId(rx.identifier, rx.extd);
    printFrame(rx);
  }
#endif

#if CARDIAG_MODE == MODE_SELFTEST
  static uint32_t lastTx = 0;
  if (millis() - lastTx >= 1000) {
    lastTx = millis();
    selfTestTick();
  }
#endif

  const uint32_t now = millis();
  if (now - g_lastStatsMs >= STATS_INTERVAL_MS) {
#if CARDIAG_MODE == MODE_POLL
    const ObdStats *s = obdStats();
    Serial.printf("-- %lu req | %lu ok | %lu timeout | %lu malformed | "
                  "%lu multiframe | last %lu ms | ECUs",
                  (unsigned long)s->requests,
                  (unsigned long)s->replies,
                  (unsigned long)s->timeouts,
                  (unsigned long)s->malformed,
                  (unsigned long)s->multiframe,
                  (unsigned long)s->lastLatencyMs);
    if (s->respondersMask == 0) {
      Serial.print(" none");
    } else {
      for (uint8_t i = 0; i < 8; i++) {
        if (s->respondersMask & (1u << i)) Serial.printf(" %03X", OBD_RESP_ID_FIRST + i);
      }
    }
    Serial.println();
    g_lastStatsMs = now;
    delay(1);
    return;
#else
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
#endif
  }

  delay(1);
}

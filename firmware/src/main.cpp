// cardiag
//
// Four modes. The mode is chosen at RUNTIME -- serial key or the BOOT button --
// and persisted in NVS, so changing it no longer means a reflash.
//
//   MODE_SELFTEST : TWAI internal loopback. *** TRANSMITS (NO_ACK) ***.
//                   Bench only, nothing connected. Proves toolchain, driver,
//                   timing config and frame handling.
//   MODE_LISTEN   : Listen-only frame dump. Never transmits, never ACKs.
//   MODE_SNIFF    : Listen-only per-ID table with sticky change marks. The
//                   readable mode; use this on a live bus.
//   MODE_POLL     : OBD-II Mode 01 polling. *** TRANSMITS ***.
//
// SAFETY: the two transmitting modes are reachable ONLY by a confirmed
// keystroke. They are never restored from NVS at boot and the button never
// selects them. SELFTEST counts as transmitting -- TWAI_MODE_NO_ACK still
// drives the bus, so running it plugged into a car would put frames onto a
// live vehicle bus.
//
// API verified 2026-08-20 against ESP-IDF v5.x legacy TWAI driver docs and the
// Autosport Labs reference example. Note the ESP-IDF v6 handle-based API
// (esp_twai.h / twai_new_node_onchip) is a DIFFERENT driver -- not this one.

#include <Arduino.h>
#include <Preferences.h>
#include "driver/twai.h"

#include "config.h"
#include "obd.h"
#include "sniffer.h"
#include "recorder.h"
#include "webui.h"

// ---------------------------------------------------------------------------
// Rolling stats. On a live bus the useful first question is not "what does
// this byte mean" but "how busy is this bus and how many distinct messages
// are on it." That tells you immediately whether you're actually connected.
// ---------------------------------------------------------------------------

static uint32_t g_frames         = 0;   // frames since mode start
static uint32_t g_framesLastTick = 0;   // for frames/sec
static uint32_t g_lastStatsMs    = 0;

// Distinct 11-bit standard IDs seen, as a bitmap. 2048 bits = 256 bytes.
static uint8_t  g_seenStd[256]   = {0};
static uint16_t g_uniqueIds      = 0;

static uint32_t      g_supported[OBD_BITMAP_WORDS] = {0};
static const ObdPid *g_pollList[16]                = {0};
static uint8_t       g_pollCount                   = 0;

static uint8_t     g_mode    = CARDIAG_MODE;
static bool        g_twaiUp  = false;
static bool        g_paused  = false;
static Preferences g_prefs;

// A transmitting mode requested but not yet confirmed, and when it was asked.
static uint8_t  g_pendingMode = 0xFF;
static uint32_t g_pendingAtMs = 0;

// The CAN drain runs in its own task so the web server's blocking client
// handling cannot delay it. g_canPause / g_canIdle are a handshake: the driver
// must not be uninstalled while the task sits inside twai_receive().
static TaskHandle_t  g_canTask  = nullptr;
static volatile bool g_canPause = false;
static volatile bool g_canIdle  = false;

static void noteId(uint32_t id, bool extended) {
  if (extended || id >= 2048) return;   // standard IDs only
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
// Modes
// ---------------------------------------------------------------------------

static const char *modeName(uint8_t m) {
  switch (m) {
    case MODE_SELFTEST: return "SELFTEST";
    case MODE_LISTEN:   return "LISTEN";
    case MODE_POLL:     return "POLL";
    case MODE_SNIFF:    return "SNIFF";
    default:            return "?";
  }
}

// The single place that decides whether a mode puts frames on the wire.
static bool modeTransmits(uint8_t m) {
  return m == MODE_SELFTEST || m == MODE_POLL;
}

static twai_mode_t twaiModeFor(uint8_t m) {
  switch (m) {
    case MODE_SELFTEST: return TWAI_MODE_NO_ACK;      // drives the bus
    case MODE_POLL:     return TWAI_MODE_NORMAL;      // drives the bus
    default:            return TWAI_MODE_LISTEN_ONLY; // provably passive
  }
}

static bool startTwai(twai_mode_t mode) {
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
      (gpio_num_t)CAN1_TX_GPIO, (gpio_num_t)CAN1_RX_GPIO, mode);

  // The default RX queue is shallow. Deepening it buys headroom on a busy bus,
  // but it is NOT the real fix -- Phase 2 drains in an ISR into a ring buffer.
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

static void blinkMode(uint8_t m) {
  // Blocking, but only on a mode change -- and a mode change restarts the
  // driver anyway, so no frames are lost that were not already lost.
  for (uint8_t i = 0; i <= m; i++) {
    digitalWrite(PIN_USER_LED, HIGH);
    delay(120);
    digitalWrite(PIN_USER_LED, LOW);
    delay(120);
  }
}

static void pollEnter() {
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
}

// Ask the receive task to step away from the driver, and wait for it to say it
// has. Bounded: worst case is one twai_receive timeout.
static void canPause() {
  if (!g_canTask) return;
  g_canPause = true;
  const uint32_t t0 = millis();
  while (!g_canIdle && millis() - t0 < CAN_RX_WAIT_MS * 3) delay(1);
}

static void canResume() { g_canPause = false; }

static void applyMode(uint8_t m, bool persist) {
  canPause();

  if (g_twaiUp) {
    twai_stop();
    twai_driver_uninstall();
    g_twaiUp = false;
  }

  g_mode           = m;
  g_frames         = 0;
  g_framesLastTick = 0;
  g_uniqueIds      = 0;
  memset(g_seenStd, 0, sizeof(g_seenStd));
  snifferReset();

  Serial.println();
  Serial.printf("Mode: %s%s\n", modeName(m),
                modeTransmits(m) ? "   *** TRANSMITS ***" : "  (passive)");

  if (!startTwai(twaiModeFor(m))) {
    Serial.println("TWAI did not start. Mode is inactive.");
    return;
  }
  g_twaiUp = true;

  // Only passive modes are remembered. A board that boots into a transmitting
  // mode because of a setting made weeks ago is exactly the failure this
  // avoids.
  if (persist && !modeTransmits(m)) {
    g_prefs.putUChar("mode", m);
  }

  if (m == MODE_POLL) pollEnter();

  blinkMode(m);
  g_lastStatsMs = millis();
  canResume();
}

// ---------------------------------------------------------------------------
// Accessors for the web layer. Declared in webui.h so that layer can read
// state and request a mode change without owning either.
// ---------------------------------------------------------------------------

uint32_t cardiagFrames() { return g_frames; }

static uint32_t statusField(bool wantMissed) {
  if (!g_twaiUp) return 0;
  twai_status_info_t st;
  twai_get_status_info(&st);
  return wantMissed ? st.rx_missed_count : st.bus_error_count;
}

uint32_t    cardiagMissed()   { return statusField(true); }
uint32_t    cardiagBusErr()   { return statusField(false); }
const char *cardiagModeName() { return modeName(g_mode); }

bool cardiagSetPassiveMode(uint8_t m) {
  // The air gap: nothing arriving over Wi-Fi may select a transmitting mode.
  if (m != MODE_LISTEN && m != MODE_SNIFF) return false;
  if (m != g_mode) applyMode(m, true);
  return true;
}

// ---------------------------------------------------------------------------

static void canTask(void *) {
  twai_message_t rx;

  for (;;) {
    if (g_canPause || !g_twaiUp) {
      g_canIdle = true;
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    g_canIdle = false;

    // MODE_POLL must NOT drain here: obdRequest() is waiting on exactly those
    // frames, and a second reader silently eats the replies.
    if (g_mode == MODE_POLL) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Blocks rather than polls, so an idle bus costs nothing.
    if (twai_receive(&rx, pdMS_TO_TICKS(CAN_RX_WAIT_MS)) != ESP_OK) continue;

    g_frames++;
    noteId(rx.identifier, rx.extd);

    // The ring always rolls, in every passive mode. You cannot decide to keep
    // the interesting thirty seconds after they have already gone past.
    recorderNoteRaw(rx);

    if (g_mode == MODE_SNIFF) {
      snifferNote(rx);   // also feeds the change log
    } else if (!g_paused) {
      printFrame(rx);
    }
  }
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

static void printHelp() {
  Serial.println();
  Serial.println("keys:  1 listen   2 sniff   3 selftest*   4 poll*   (* transmits, asks to confirm)");
  Serial.println("       c clear marks   p pause   r reset table   w wifi ap   h help");
  Serial.println("       l start/stop change log   k clear change log");
  Serial.println("button: short press cycles LISTEN <-> SNIFF (passive modes only)");
  Serial.println();
}

static void requestMode(uint8_t m) {
  if (m == g_mode) {
    Serial.printf("already in %s\n", modeName(m));
    return;
  }
  if (modeTransmits(m)) {
    g_pendingMode = m;
    g_pendingAtMs = millis();
    Serial.printf("\n*** %s TRANSMITS onto the bus. Press 'y' within %us to confirm. ***\n",
                  modeName(m), (unsigned)(POLL_CONFIRM_WINDOW_MS / 1000));
    return;
  }
  applyMode(m, true);
}

static void handleKeys() {
  while (Serial.available()) {
    const int ch = Serial.read();

    if (g_pendingMode != 0xFF) {
      if (ch == 'y' || ch == 'Y') {
        const uint8_t m = g_pendingMode;
        g_pendingMode = 0xFF;
        applyMode(m, true);
      } else {
        g_pendingMode = 0xFF;
        Serial.println("cancelled.");
      }
      continue;
    }

    switch (ch) {
      case '1': requestMode(MODE_LISTEN);   break;
      case '2': requestMode(MODE_SNIFF);    break;
      case '3': requestMode(MODE_SELFTEST); break;
      case '4': requestMode(MODE_POLL);     break;
      case 'c': case 'C':
        snifferClearMarks();
        Serial.println("-- marks cleared, baseline re-taken --");
        break;
      case 'r': case 'R':
        snifferReset();
        Serial.println("-- table reset --");
        break;
      case 'p': case 'P':
        g_paused = !g_paused;
        Serial.printf("-- %s --\n", g_paused ? "paused" : "resumed");
        break;
      case 'w': case 'W':
        if (webuiRunning()) {
          webuiStop();
          g_prefs.putBool("ap", false);
        } else {
          webuiStart();
          g_prefs.putBool("ap", true);
        }
        break;
      case 'l': case 'L':
        if (recorderRunning()) {
          recorderStop();
          Serial.printf("-- change log STOPPED, %lu entries --\n",
                        (unsigned long)recorderChangeStored());
        } else {
          recorderStart();
          Serial.println("-- change log RUNNING (needs SNIFF mode to fill) --");
        }
        break;
      case 'k': case 'K':
        recorderClear();
        Serial.println("-- change log cleared --");
        break;
      case 'h': case 'H': case '?':
        printHelp();
        break;
      default: break;
    }
  }

  if (g_pendingMode != 0xFF &&
      millis() - g_pendingAtMs > POLL_CONFIRM_WINDOW_MS) {
    g_pendingMode = 0xFF;
    Serial.println("confirm window expired; staying passive.");
  }
}

static void handleButton() {
  static bool     wasDown  = false;
  static uint32_t lastEdge = 0;

  const bool isDown = (digitalRead(PIN_MODE_BUTTON) == LOW);
  if (isDown == wasDown) return;
  if (millis() - lastEdge < BUTTON_DEBOUNCE_MS) return;

  lastEdge = millis();
  wasDown  = isDown;

  // Act on release, so a long hold for the bootloader is not a mode change.
  if (isDown) return;

  // Passive modes only. The button must never be able to start transmitting.
  applyMode(g_mode == MODE_SNIFF ? MODE_LISTEN : MODE_SNIFF, true);
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  // Native USB CDC blocks on write when no host is draining the TX buffer.
  // In the car there is no laptop attached, so an unguarded Serial.print can
  // stall the whole loop. 0 = never block; drop instead.
  Serial.setTxTimeoutMs(0);
  delay(2000);   // let USB CDC enumerate before the first print

  pinMode(PIN_USER_LED, OUTPUT);
  digitalWrite(PIN_USER_LED, LOW);
  pinMode(PIN_MODE_BUTTON, INPUT_PULLUP);

  Serial.println();
  Serial.println("cardiag");

  snifferBegin();
  recorderBegin();
  g_prefs.begin("cardiag", false);
  uint8_t stored = g_prefs.getUChar("mode", CARDIAG_MODE);
  if (modeTransmits(stored)) {
    // Refuse to resume a transmitting mode unattended.
    Serial.printf("stored mode %s transmits; starting in LISTEN instead.\n",
                  modeName(stored));
    stored = MODE_LISTEN;
  }

  Serial.println("Bitrate: 500 kbit/s");
  printHelp();

  applyMode(stored, false);

  xTaskCreatePinnedToCore(canTask, "can", CAN_TASK_STACK, nullptr,
                          CAN_TASK_PRIO, &g_canTask, CAN_TASK_CORE);

  // On by default: the whole point is that the board is usable with no laptop,
  // and a board that needs one to switch its radio on would defeat that.
  if (g_prefs.getBool("ap", true)) webuiStart();
}

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

// Transmit a frame to ourselves once per second and verify it comes back.
// If TX succeeds but nothing is received, the driver is running but the
// self-reception request is not being honoured -- a config problem, not wiring.
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

void loop() {
  handleKeys();
  handleButton();
  webuiLoop();

  if (!g_twaiUp) { delay(10); return; }

  // Receiving happens in canTask. This loop only transmits, prints and serves.
  if (g_mode == MODE_POLL) {
    static uint32_t lastPoll = 0;
    if (millis() - lastPoll >= OBD_POLL_INTERVAL_MS) {
      lastPoll = millis();
      pollTick();
    }
  }

  if (g_mode == MODE_SELFTEST) {
    static uint32_t lastTx = 0;
    if (millis() - lastTx >= 1000) {
      lastTx = millis();
      selfTestTick();
    }
  }

  const uint32_t now      = millis();
  const uint32_t interval =
      (g_mode == MODE_SNIFF) ? SNIFF_PRINT_INTERVAL_MS : STATS_INTERVAL_MS;

  if (now - g_lastStatsMs >= interval) {
    twai_status_info_t st;
    twai_get_status_info(&st);

    if (g_mode == MODE_POLL) {
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
    } else if (g_mode == MODE_SNIFF) {
      if (!g_paused) {
        snifferPrint(g_frames, st.rx_missed_count, st.bus_error_count);
      }
    } else {
      const uint32_t elapsed = now - g_lastStatsMs;
      const uint32_t fps     = (g_frames - g_framesLastTick) * 1000UL / elapsed;

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
    }

    g_lastStatsMs = now;
  }

  delay(1);
}

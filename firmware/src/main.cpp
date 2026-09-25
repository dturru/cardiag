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
#include "session.h"
#include "hublink.h"
#include "hubstream.h"
#include "selftest_profile.h"
#include "filestore.h"
#include "transceiver.h"
#include "bootguard.h"
#include "bootguard_rt.h"
#include <esp_task_wdt.h>
#include <esp_system.h>   // esp_reset_reason() -- why this boot was a boot
#include <esp_core_dump.h>  // and, on a crash, WHERE it died

static void selfTestReset();

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
  // Raised from 32 when the hub work landed: WebServer and the UDP stream
  // now share core 1 with the CAN task, so the queue has to absorb a
  // scheduling hiccup rather than drop frames. 128 x 16 B is ~2 kB.
  g.rx_queue_len = 128;

  // Default is 5. SELFTEST now emits ~237 frames/s across 14 ids, and several
  // can come due in the same loop pass, so a queue of 5 would refuse frames
  // for scheduling reasons and the refusals would look like a driver problem.
  // 32 x 16 B is ~512 B. Only SELFTEST and POLL ever transmit.
  g.tx_queue_len = 32;

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

  // Files are tagged with the mode that produced them and never span two, so
  // a synthetic bench run cannot end up inside a real capture's file.
  filestoreSetMode(m);
  filestoreSetEnabled(true);

  if (m == MODE_SELFTEST) {
    selfTestReset();
    // Seed the fast list so the bench exercises it without needing the hub to
    // configure anything first. This is the ONE place firmware picks a fast id
    // by itself, and it is gated on SELFTEST: on a real bus the list stays
    // empty until the hub sets it, because the logger does not know what any
    // id means and must not pretend to.
    const uint32_t fast = SELFTEST_FAST_ID;
    hubstreamSetFastIds(&fast, 1);
    Serial.printf("[selftest] %u ids, ~%u frames/s, fast id 0x%03X @ %u Hz\n",
                  (unsigned)SELFTEST_ID_COUNT, 237u,
                  (unsigned)SELFTEST_FAST_ID, (unsigned)HUB_FAST_HZ);
  } else {
    // Leaving SELFTEST drops the synthetic fast id. Carrying a bench id into a
    // live capture would stream a nonexistent signal at 20 Hz.
    hubstreamSetFastIds(nullptr, 0);
  }

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

    // Bus activity re-arms the clean-key-off close. One volatile store; this
    // is the ~1,500 frames/s path.
    filestoreNoteBusActivity();

    // The ring always rolls, in every passive mode. You cannot decide to keep
    // the interesting thirty seconds after they have already gone past.
    recorderNoteRaw(rx);

    if (g_mode == MODE_SNIFF) {
      snifferNote(rx);   // also feeds the change log
    } else {
      // The per-ID table is what the hub's UDP snapshot reads, so keep it
      // current in LISTEN and SELFTEST as well. feedRecorder=false: loopback
      // and listen frames must not be written into the change log, which is a
      // record of the CAR, not of the bench.
      //
      // ⚠ The ONE exception is env:esp32-can-x2-fstest, which exists so the
      // retention path is reachable without a car: Tier A is fed only by the
      // change log, so the Tier A cap -- and the unacked eviction it triggers
      // -- cannot otherwise be exercised on a desk. The discipline above is
      // NOT relaxed in any shipping build.
#if defined(FSTEST_BENCH_CHANGELOG) && FSTEST_BENCH_CHANGELOG
      snifferNote(rx, g_mode == MODE_SELFTEST);
#else
      snifferNote(rx, false);
#endif
      if (!g_paused) printFrame(rx);
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
          // 'w' is the manual radio toggle and stays AP-only: it is the
          // "I want the standalone UI now" key, not a hub-join request.
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

  // ⭐ WHY THIS BOOT WAS A BOOT. Nothing printed it before, although the
  // handoff notes twice claimed "the firmware prints the reset reason on boot
  // and settles it in one line" -- it did not, which is why the 2026-09-24
  // mid-run reboot went three sessions without an answer. A soak that counts
  // reboots but cannot say WHY is a reboot counter, not a diagnosis.
  //
  // BROWNOUT here means the supply sagged: on the bench that is the dupont
  // harness or the USB cable, not a firmware fault. TASK_WDT, INT_WDT and
  // PANIC are ours. Keeping the two classes separable is the whole point.
  {
    const esp_reset_reason_t rr = esp_reset_reason();
    const char *name;
    switch (rr) {
      case ESP_RST_POWERON:   name = "POWERON";   break;
      case ESP_RST_EXT:       name = "EXT";       break;
      case ESP_RST_SW:        name = "SW";        break;
      case ESP_RST_PANIC:     name = "PANIC";     break;
      case ESP_RST_INT_WDT:   name = "INT_WDT";   break;
      case ESP_RST_TASK_WDT:  name = "TASK_WDT";  break;
      case ESP_RST_WDT:       name = "WDT";       break;
      case ESP_RST_DEEPSLEEP: name = "DEEPSLEEP"; break;
      case ESP_RST_BROWNOUT:  name = "BROWNOUT";  break;
      case ESP_RST_SDIO:      name = "SDIO";      break;
      // ⚠️ THE TAIL OF THE ENUM IS NOT OPTIONAL. Reason 11 (USB) is what a
      // host-side esptool reset produces, and it showed up on the very first
      // bench check -- printed as "UNKNOWN (11)", which reads exactly like
      // firmware too old to classify. On an overnight soak that would send
      // someone chasing a stale flash instead of a USB event. PWR_GLITCH and
      // CPU_LOCKUP matter for the same reason: both are real findings and
      // neither is "unknown".
      case ESP_RST_USB:       name = "USB";       break;
      case ESP_RST_JTAG:      name = "JTAG";      break;
      case ESP_RST_EFUSE:     name = "EFUSE";     break;
      case ESP_RST_PWR_GLITCH: name = "PWR_GLITCH"; break;
      case ESP_RST_CPU_LOCKUP: name = "CPU_LOCKUP"; break;
      default:                name = "UNKNOWN";   break;
    }
    Serial.printf("[boot] RESET REASON: %s (%d)\n", name, (int)rr);

    // ⭐ A FAIL MUST ARRIVE WITH ITS EVIDENCE. The reason alone says a crash
    // happened; the coredump says WHERE. Without this an overnight soak
    // reports "1 TASK_WDT at cycle 137" and the next session still has to
    // reproduce it to learn anything.
    //
    // Only on the four reasons that leave a dump. A BROWNOUT does not crash
    // the firmware -- the supply went away -- so there is nothing to read, and
    // reading it anyway would print a stale dump from an older crash and
    // attribute it to the wrong boot.
    if (rr == ESP_RST_PANIC || rr == ESP_RST_TASK_WDT ||
        rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT) {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
      // Heap, not stack: the struct carries a 16-entry backtrace plus a
      // sha256 string, and setup() runs on the Arduino task's stack.
      esp_core_dump_summary_t *s =
          (esp_core_dump_summary_t *)malloc(sizeof(esp_core_dump_summary_t));
      if (s && esp_core_dump_get_summary(s) == ESP_OK) {
        Serial.printf("[boot] COREDUMP: task='%s' pc=0x%08lx depth=%u%s\n",
                      s->exc_task, (unsigned long)s->exc_pc,
                      (unsigned)s->exc_bt_info.depth,
                      s->exc_bt_info.corrupted ? " (BACKTRACE CORRUPT)" : "");
        // Six frames is enough to name the call path and still fit one line.
        const uint32_t n = s->exc_bt_info.depth < 6 ? s->exc_bt_info.depth : 6;
        Serial.print("[boot] COREDUMP BT:");
        for (uint32_t i = 0; i < n; i++) {
          Serial.printf(" 0x%08lx", (unsigned long)s->exc_bt_info.bt[i]);
        }
        Serial.println();
      } else {
        Serial.println("[boot] COREDUMP: none readable (crash may predate "
                       "coredump support, or the dump was already erased)");
      }
      free(s);
#else
      Serial.println("[boot] COREDUMP: not built in "
                     "(ESP_COREDUMP_ENABLE_TO_FLASH / DATA_FORMAT_ELF off)");
#endif
      // 🔑 ERASE IT. The partition holds ONE dump: with a stale dump in place
      // the NEXT crash has nowhere to go, and every later boot would reprint
      // this same summary as though it were fresh. A soak that crashes twice
      // must be able to show both.
      const esp_err_t er = esp_core_dump_image_erase();
      Serial.printf("[boot] COREDUMP erased: %s\n",
                    er == ESP_OK ? "ok" : esp_err_to_name(er));
    }
  }

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

  // Identity first: boot_id and device_id must exist before any packet, log
  // line, file name or API response can reference them. This moved AHEAD of
  // applyMode() in Phase B -- filenames carry boot_id, so the filestore cannot
  // open anything until the session exists, and applyMode() now opens files.
  sessionBegin();

  // Persistence. A mount failure is NOT fatal: rule 1 says the logger is
  // standalone, and a board that refuses to log to PSRAM because its flash is
  // unhappy would be worse than one that says so and carries on.
  //
  // ========================================================================
  // ⭐ BOUNDED, AND REMEMBERED IF IT IS NOT.
  //
  // 🐛 2026-09-25: after a 200-cycle soak this call took 187 s (a quadratic
  // directory scan), with no watchdog, because the watchdog was deliberately
  // armed only AFTER it -- to protect an automatic format that no longer
  // exists. A slower disk, or a real hang, would have held the board here
  // forever, awake on an unswitched feed with no radio up to say so.
  //
  // Now: the watchdog is armed FIRST with the boot budget FS_BOOT_WDT_S, the
  // attempt is recorded (RTC + NVS) before the start and cleared only after it
  // finishes, and BG_FAIL_LIMIT unfinished starts in a row boot SAFE MODE:
  // no filestore, radio and API up, `filestore_failed` on /api/v1/session,
  // and a person or the hub decides between retry and erase. Never a format
  // on its own.
  // ========================================================================
  static_assert(FS_BOOT_WDT_S >= WDT_TIMEOUT_S,
                "FS_BOOT_WDT_S is floored at WDT_TIMEOUT_S; see config.h");
  {
    const uint8_t prior = bootguardBegin();
    if (bgShouldSafeMode(prior)) {
      filestoreBeginSafeMode(prior);
    } else {
      bootguardArmTaskWdt(FS_BOOT_WDT_S);
      Serial.printf("[wdt] boot watchdog armed, %us, covering the filestore "
                    "start\n", (unsigned)FS_BOOT_WDT_S);
      bootguardMarkStart(prior);
      // A mount failure returns false WITHOUT clearing: it counts as a failed
      // start, so a partition that never mounts reaches safe mode instead of
      // being retried (or, as before, formatted) on every boot.
      if (filestoreBegin()) bootguardClear();
    }
  }

  // ========================================================================
  // FAILSAFE LAYER 2 -- BOOT-TIME CHECK.
  //
  // This is what catches the reset layer 1 just caused. Waking up, finding a
  // dead car, and cheerfully staying awake would turn a watchdog SAVE into
  // exactly the battery drain the watchdog fired to prevent.
  //
  // Any leftover .part from the boot that just died is closed here as a side
  // effect of the same call, so a crash artifact stops being open the moment
  // the board comes back rather than lingering until the next mode change.
  //
  // Deliberately AFTER filestoreBegin() -- it needs the scan to know what is
  // open -- and BEFORE the radio starts, because there is no point joining a
  // network on a car that left twenty minutes ago.
  //
  // ⚠ On the dev board this can only log; there is no INH path to switch. On
  // the carrier it removes power here.
  //
  // ========================================================================
  // FAILSAFE LAYER 1 -- HARDWARE TASK WATCHDOG.
  //
  // The TCAN1043**G** on the carrier has NO tINACTIVE / SWE failsafe (the A
  // variant does; ours does not), and INH sits on the UNSWITCHED OBD pin 16.
  // So nothing in hardware will ever turn this board off, and a firmware hang
  // with INH asserted is an ESP32 awake on the car battery until the battery
  // is flat. The other two layers are code and cannot help when the code is
  // what stopped running; this one can.
  //
  // Already armed (with the boot budget) above; this narrows it to the loop
  // window. There is no longer any unwatched stretch of setup(). Everything
  // slow that runs after this point feeds the watchdog as it makes progress
  // -- see the esp_task_wdt_reset() calls in filestore.cpp's retention,
  // hydration and download paths. A watchdog that fires during a legitimate
  // long operation is worse than no watchdog.
  // ========================================================================
  bootguardArmTaskWdt(WDT_TIMEOUT_S);
  Serial.printf("[wdt] task watchdog armed, %us (no hardware failsafe on "
                "the TCAN1043G -- this is the only one that survives a "
                "hang)\n", (unsigned)WDT_TIMEOUT_S);

  transceiverRequestSleep();

  applyMode(stored, false);

  xTaskCreatePinnedToCore(canTask, "can", CAN_TASK_STACK, nullptr,
                          CAN_TASK_PRIO, &g_canTask, CAN_TASK_CORE);

  // On by default: the whole point is that the board is usable with no laptop,
  // and a board that needs one to switch its radio on would defeat that.
  //
  // hublinkBegin() tries the hub network FIRST and falls back to webuiStart(),
  // so the standalone behaviour above is preserved exactly. If the radio was
  // deliberately switched off with 'w', respect that and start nothing --
  // EXCEPT in safe mode, where the radio is the only way anyone can reach the
  // board to retry or erase. A safe mode nobody can talk to is just a hang.
  if (g_prefs.getBool("ap", true) || filestoreSafeMode()) {
    hublinkBegin();
    hubstreamBegin();
  }
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
// ---------------------------------------------------------------------------
// SELFTEST traffic generator.
//
// Emits the 14 ids from the 2026-09-08 Civic capture at their measured rates
// (see selftest_profile.h for why those numbers and what they are not).
//
// PAYLOAD SHAPE, and why it is not just a counter:
//
//   d0            rolling counter, moves EVERY frame -> the sniffer classifies
//                 it as a heartbeat and suppresses it
//   d[dlc-1]      running checksum, also every frame -> also a heartbeat
//   d1            steps once per SELFTEST_SIGNAL_STEP_MS -> the only byte that
//                 survives the filter, so it is what reaches the change log
//   rest          constant, per id
//
// That mix is the part that matters. A payload where every byte moves would
// make the heartbeat filter look like it works while never testing that a real
// signal gets through it; a payload where nothing moves would never populate
// the change log at all. Both failures were reachable with the old one-id
// workload and neither would have been visible.
// ---------------------------------------------------------------------------

static uint32_t g_stNext[SELFTEST_ID_COUNT];
static uint8_t  g_stCounter[SELFTEST_ID_COUNT];
static bool     g_stOnce[SELFTEST_ID_COUNT];
static uint32_t g_stFrames  = 0;
static uint32_t g_stTxFails = 0;
static uint32_t g_stResyncs = 0;

static void selfTestReset() {
  const uint32_t now = millis();
  for (uint16_t i = 0; i < SELFTEST_ID_COUNT; i++) {
    // Stagger the phases. Starting every id at the same instant would put all
    // 14 transmits in one loop pass forever, which is a burst pattern the real
    // bus does not have and which would hide a tx-queue problem behind a
    // permanent worst case.
    g_stNext[i]    = now + (i * 7);
    g_stCounter[i] = 0;
    g_stOnce[i]    = false;
  }
  g_stFrames = g_stTxFails = g_stResyncs = 0;
}

static void selfTestTick() {
  const uint32_t now = millis();

  for (uint16_t i = 0; i < SELFTEST_ID_COUNT; i++) {
    const SelfTestId &p = SELFTEST_IDS[i];

    if (p.periodMs == 0) {
      if (g_stOnce[i]) continue;      // emitted once per run, by design
      g_stOnce[i] = true;
    } else {
      if ((int32_t)(now - g_stNext[i]) < 0) continue;
      g_stNext[i] += p.periodMs;
      // If the scheduler fell behind (a long web request, a Wi-Fi transition),
      // do NOT try to catch up: replaying the backlog would emit a burst that
      // never happens on a real bus and would misrepresent the rate. Resync
      // and count it, so the log says how often it happened.
      if ((int32_t)(now - g_stNext[i]) > (int32_t)p.periodMs) {
        g_stNext[i] = now + p.periodMs;
        g_stResyncs++;
      }
    }

    twai_message_t tx = {};     // zero-init: clears extd/rtr/ss/dlc_non_comp
    tx.identifier       = p.id;
    tx.self             = 1;    // self-reception request
    tx.data_length_code = p.dlc;

    const uint8_t ctr = g_stCounter[i]++;
    tx.data[0] = ctr;
    if (p.dlc > 2) tx.data[1] = (uint8_t)((now / SELFTEST_SIGNAL_STEP_MS) + i);
    for (uint8_t b = 2; b + 1 < p.dlc; b++) tx.data[b] = (uint8_t)(p.id + b);
    if (p.dlc > 1) {
      uint8_t sum = 0;
      for (uint8_t b = 0; b + 1 < p.dlc; b++) sum = (uint8_t)(sum + tx.data[b]);
      tx.data[p.dlc - 1] = (uint8_t)(~sum);
    }

    // Non-blocking. At ~237 frames/s a blocking transmit would stall loop()
    // behind the tx queue, which would slow the web server and the UDP stream
    // -- the very things this workload exists to exercise. A refused frame is
    // counted instead, and a rising count is a real finding.
    if (twai_transmit(&tx, 0) != ESP_OK) g_stTxFails++;
    else g_stFrames++;
  }
}

void loop() {
  // Layer 1: feed the watchdog. If loop() stops running, the chip resets and
  // layer 2 decides what to do about it on the way back up.
  esp_task_wdt_reset();

  // FAILSAFE LAYER 3 -- MAX-AWAKE BACKSTOP. Checked every pass and cheap: it
  // is two integer comparisons until the bus has actually been quiet for
  // CAN_MAX_AWAKE_MS. transceiverRequestSleep() owns the invariant, including
  // closing any file that is somehow still open at the backstop.
  if (filestoreBusQuietMs() >= CAN_MAX_AWAKE_MS) transceiverRequestSleep();

  handleKeys();
  handleButton();
  webuiLoop();
  hublinkLoop();
  hubstreamLoop();
  filestoreLoop();
  // millis() wraps at ~49.7 d; this closes the session and starts a new
  // boot_id so relative time stays monotonic within a session.
  if (sessionTick()) hubstreamRequestFullSnapshot();

  if (!g_twaiUp) { delay(10); return; }

  // Receiving happens in canTask. This loop only transmits, prints and serves.
  if (g_mode == MODE_POLL) {
    static uint32_t lastPoll = 0;
    if (millis() - lastPoll >= OBD_POLL_INTERVAL_MS) {
      lastPoll = millis();
      pollTick();
    }
  }

  if (g_mode == MODE_SELFTEST) selfTestTick();

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

      if (g_mode == MODE_SELFTEST) {
        Serial.printf("-- selftest tx=%lu txfail=%lu resync=%lu | "
                      "udp pkts=%lu recs=%lu fast_pkts=%lu fast_recs=%lu\n",
                      (unsigned long)g_stFrames,
                      (unsigned long)g_stTxFails,
                      (unsigned long)g_stResyncs,
                      (unsigned long)hubstreamPacketsSent(),
                      (unsigned long)hubstreamRecordsSent(),
                      (unsigned long)hubstreamFastPacketsSent(),
                      (unsigned long)hubstreamFastRecordsSent());
      }

      g_framesLastTick = g_frames;
    }

    g_lastStatsMs = now;
  }

  delay(1);
}

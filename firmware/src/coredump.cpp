#include <Arduino.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_partition.h>
#include <esp_core_dump.h>
#include <esp_task_wdt.h>
#include <mbedtls/sha256.h>

#include "coredump.h"
#include "cdelf.h"
#include "hubapi.h"

// Built only when the core supports it. The same guard the old boot-time
// summary used, so a build without flash coredumps compiles and says so.
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
#define CD_BUILT 1
#else
#define CD_BUILT 0
#endif

static bool     s_present = false;
static bool     s_isElf = false;
static uint32_t s_imageOff = 0;    // image start, relative to the partition
static uint32_t s_imageLen = 0;
static uint32_t s_serveOff = 0;    // what the default GET serves, relative to the image
static uint32_t s_serveLen = 0;
static char     s_sha[65] = "";
static const esp_partition_t *s_part = nullptr;

// Lifetime, NVS-backed. See coredump.h.
static Preferences s_prefs;
static uint32_t s_crashes = 0;
static uint32_t s_acked = 0;
static char     s_lastReason[12] = "";

static const char *crashReasonName(int rr) {
  switch (rr) {
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_WDT:      return "WDT";
    default:               return nullptr;   // not a crash
  }
}

static inline void wdtFeedIfArmed() {
  if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
}

// Reads relative to the START OF THE STORED IMAGE, which is what cdelf wants.
static bool readImage(void *, uint32_t off, void *buf, uint32_t len) {
  if (!s_part) return false;
  if ((uint64_t)off + len > s_imageLen) return false;
  return esp_partition_read(s_part, s_imageOff + off, buf, len) == ESP_OK;
}

static bool hashRange(uint32_t off, uint32_t len, char out[65]) {
  mbedtls_sha256_context c;
  mbedtls_sha256_init(&c);
  mbedtls_sha256_starts(&c, 0);
  uint8_t buf[512];
  bool ok = true;
  for (uint32_t done = 0; done < len;) {
    const uint32_t n = (len - done) < sizeof(buf) ? (len - done) : sizeof(buf);
    if (!readImage(nullptr, off + done, buf, n)) { ok = false; break; }
    mbedtls_sha256_update(&c, buf, n);
    done += n;
  }
  uint8_t d[32];
  mbedtls_sha256_finish(&c, d);
  mbedtls_sha256_free(&c);
  if (!ok) { out[0] = 0; return false; }
  for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
  return true;
}

static void printSummary() {
#if CD_BUILT
  // Heap, not stack: the struct carries a 16-entry backtrace plus a sha256
  // string, and setup() runs on the Arduino task's stack.
  esp_core_dump_summary_t *s =
      (esp_core_dump_summary_t *)malloc(sizeof(esp_core_dump_summary_t));
  if (s && esp_core_dump_get_summary(s) == ESP_OK) {
    Serial.printf("[boot] COREDUMP: task='%s' pc=0x%08lx depth=%u%s\n",
                  s->exc_task, (unsigned long)s->exc_pc,
                  (unsigned)s->exc_bt_info.depth,
                  s->exc_bt_info.corrupted ? " (BACKTRACE CORRUPT)" : "");
    const uint32_t n = s->exc_bt_info.depth < 6 ? s->exc_bt_info.depth : 6;
    Serial.print("[boot] COREDUMP BT:");
    for (uint32_t i = 0; i < n; i++) {
      Serial.printf(" 0x%08lx", (unsigned long)s->exc_bt_info.bt[i]);
    }
    Serial.println();
  } else {
    Serial.println("[boot] COREDUMP: stored image present but the summary "
                   "is not readable");
  }
  free(s);
#endif
}

void coredumpBegin(int resetReason) {
  s_present = false;
  s_sha[0] = 0;

  // Count the crash FIRST, before anything that could fail, and whether or
  // not a dump survived: the count is what remains when the dump does not.
  s_prefs.begin("cardiagcd", false);
  s_crashes = s_prefs.getULong("crashes", 0);
  s_acked = s_prefs.getULong("acked", 0);
  s_prefs.getString("lastrr", s_lastReason, sizeof(s_lastReason));
  if (const char *why = crashReasonName(resetReason)) {
    s_crashes++;
    s_prefs.putULong("crashes", s_crashes);
    snprintf(s_lastReason, sizeof(s_lastReason), "%s", why);
    s_prefs.putString("lastrr", s_lastReason);
  }
  Serial.printf("[boot] CRASHES (lifetime): %lu, dumps acked by hub: %lu%s%s\n",
                (unsigned long)s_crashes, (unsigned long)s_acked,
                s_lastReason[0] ? ", last: " : "", s_lastReason);
#if CD_BUILT
  const bool crashed = resetReason == ESP_RST_PANIC ||
                       resetReason == ESP_RST_TASK_WDT ||
                       resetReason == ESP_RST_INT_WDT ||
                       resetReason == ESP_RST_WDT;

  s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  size_t addr = 0, size = 0;
  if (!s_part || esp_core_dump_image_get(&addr, &size) != ESP_OK || !size ||
      addr < s_part->address) {
    if (crashed) {
      Serial.println("[boot] COREDUMP: none readable (crash may predate "
                     "coredump support, or the write did not complete)");
    }
    return;
  }
  s_imageOff = (uint32_t)(addr - s_part->address);
  s_imageLen = (uint32_t)size;

  const CdReader r = {nullptr, readImage};
  uint32_t eo = 0, el = 0;
  s_isElf = cdElfExtent(&r, s_imageLen, &eo, &el);
  s_serveOff = s_isElf ? eo : 0;
  s_serveLen = s_isElf ? el : s_imageLen;
  if (!hashRange(s_serveOff, s_serveLen, s_sha)) {
    Serial.println("[boot] COREDUMP: stored image could not be read; not "
                   "offering it");
    return;
  }
  s_present = true;

  // ⭐ NOT ERASED. It stays until POST /api/v1/coredump/ack names this sha.
  //
  // The old code erased here so "the next crash has somewhere to go". It
  // does not need to: ESP-IDF's flash coredump writer overwrites the stored
  // dump on every crash, so the policy is LATEST WINS. What that loses is the
  // older dump when two crashes happen before a fetch -- which is what the
  // NVS crash counter above is for: the crash is still counted.
  Serial.printf("[boot] COREDUMP PRESENT: %s, %lu B, sha256 %.12s... -- kept "
                "until fetched and acked (GET /api/v1/coredump). This boot's "
                "reset was %s; the dump may be from an EARLIER boot.\n",
                s_isElf ? "ELF" : "raw image (no ELF found)",
                (unsigned long)s_serveLen, s_sha,
                crashed ? "a crash" : "not a crash");
  printSummary();
#else
  (void)resetReason;
  Serial.println("[boot] COREDUMP: not built in "
                 "(ESP_COREDUMP_ENABLE_TO_FLASH / DATA_FORMAT_ELF off)");
#endif
}

bool coredumpPresent() { return s_present; }
uint32_t coredumpCrashesTotal() { return s_crashes; }
uint32_t coredumpDumpsAcked() { return s_acked; }
const char *coredumpLastCrashReason() { return s_lastReason; }
uint32_t coredumpBytes() { return s_present ? s_serveLen : 0; }
const char *coredumpSha256Hex() { return s_present ? s_sha : ""; }
const char *coredumpFormat() { return s_isElf ? "elf" : "raw"; }

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

static void handleGet(WebServer &srv) {
  if (!hubapiTokenOk(srv)) {
    srv.send(401, "application/json", "{\"error\":\"bad or missing X-Hub-Token\"}");
    return;
  }
  if (!s_present) {
    srv.send(404, "application/json", "{\"error\":\"no coredump stored\"}");
    return;
  }
  // Default: exactly the bytes the sha covers. ?format=raw: the whole stored
  // image, header and checksum included -- for espcoredump.py -t raw, or when
  // the ELF could not be located.
  const bool raw = srv.arg("format") == "raw";
  const uint32_t off = raw ? 0 : s_serveOff;
  const uint32_t len = raw ? s_imageLen : s_serveLen;

  srv.sendHeader("X-Coredump-Format", raw ? "raw" : coredumpFormat());
  if (!raw || !s_isElf) srv.sendHeader("X-Coredump-Sha256", s_sha);
  srv.setContentLength(len);
  srv.send(200, "application/octet-stream", "");

  uint8_t buf[1024];
  for (uint32_t done = 0; done < len;) {
    const uint32_t n = (len - done) < sizeof(buf) ? (len - done) : sizeof(buf);
    if (!readImage(nullptr, off + done, buf, n)) break;   // short body; the hub's sha check catches it
    srv.sendContent((const char *)buf, n);
    done += n;
    // Bought with progress, as in the file download: a stall inside one
    // sendContent() still trips the watchdog.
    wdtFeedIfArmed();
  }
}

// POST /api/v1/coredump/ack  {"sha256":"<64 hex>"}
static void handleAck(WebServer &srv) {
  if (!hubapiTokenOk(srv)) {
    srv.send(401, "application/json", "{\"error\":\"bad or missing X-Hub-Token\"}");
    return;
  }
  const String body = srv.arg("plain");
  const int k = body.indexOf("\"sha256\"");
  const int q = (k >= 0) ? body.indexOf('"', body.indexOf(':', k) + 1) : -1;
  const String got = (q >= 0) ? body.substring(q + 1, q + 65) : String();
  if (got.length() != 64) {
    srv.send(400, "application/json", "{\"error\":\"need {\\\"sha256\\\":\\\"<64 hex>\\\"}\"}");
    return;
  }
  // Idempotent: acking a dump that is already gone is success, like a
  // repeated file-watermark ack.
  if (!s_present) {
    srv.send(200, "application/json",
             "{\"ok\":true,\"erased\":false,\"present\":false}");
    return;
  }
  // Only the dump the hub actually holds may be erased. A different sha means
  // the hub is acking an older fetch; erasing on it would destroy a dump
  // nobody has.
  if (!got.equalsIgnoreCase(s_sha)) {
    srv.send(409, "application/json",
             "{\"error\":\"sha256 does not match the stored dump\"}");
    return;
  }
#if CD_BUILT
  const esp_err_t er = esp_core_dump_image_erase();
#else
  const esp_err_t er = ESP_FAIL;
#endif
  if (er != ESP_OK) {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"error\":\"erase failed: %s\"}", esp_err_to_name(er));
    srv.send(500, "application/json", buf);
    return;
  }
  Serial.printf("[coredump] acked by hub (sha256 %.12s...) and erased\n", s_sha);
  s_acked++;
  s_prefs.putULong("acked", s_acked);
  s_present = false;
  s_sha[0] = 0;
  srv.send(200, "application/json", "{\"ok\":true,\"erased\":true,\"present\":false}");
}

void coredumpRegister(WebServer &srv) {
  // Literal /ack registered FIRST, the same ordering rule as /files/ack.
  srv.on("/api/v1/coredump/ack", HTTP_POST, [&srv]() { handleAck(srv); });
  srv.on("/api/v1/coredump", HTTP_GET, [&srv]() { handleGet(srv); });
}

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "recorder.h"
#include "config.h"

// 16 bytes on the nose: no padding, so capacity arithmetic is exact and a
// download is trivially seekable.
struct Rec {
  uint32_t ms;
  uint16_t id;
  uint8_t  dlc;
  uint8_t  flags;      // bit0 extended, bit1..: unused
  uint8_t  data[8];
};
static_assert(sizeof(Rec) == 16, "Rec must stay 16 bytes");

// The change log stores the same record plus which bytes moved. Kept as a
// parallel array rather than a wider struct so Rec stays 16 bytes for both.
static Rec     *g_raw      = nullptr;
static size_t   g_rawCap   = 0;
static uint32_t g_rawHead  = 0;   // next write index
static uint32_t g_rawTotal = 0;   // frames ever seen

static Rec     *g_chg      = nullptr;
static uint8_t *g_chgMask  = nullptr;
static size_t   g_chgCap   = 0;
static uint32_t g_chgCount = 0;
static uint32_t g_chgDrop  = 0;

static bool g_psram   = false;
static bool g_running = false;
static bool g_frozen  = false;

// Written by the CAN task, read by the web server on the other core.
static SemaphoreHandle_t g_lock = nullptr;
static inline void lockRec()   { if (g_lock) xSemaphoreTake(g_lock, portMAX_DELAY); }
static inline void unlockRec() { if (g_lock) xSemaphoreGive(g_lock); }

// ---------------------------------------------------------------------------

void recorderBegin() {
  if (!g_lock) g_lock = xSemaphoreCreateMutex();

  g_psram = (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0);

  const size_t rawBytes = g_psram ? REC_RAW_BYTES_PSRAM : REC_RAW_BYTES_FALLBACK;
  const size_t chgBytes = g_psram ? REC_CHG_BYTES_PSRAM : REC_CHG_BYTES_FALLBACK;

  const uint32_t caps = g_psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_8BIT;

  g_raw = (Rec *)heap_caps_malloc(rawBytes, caps);
  g_chg = (Rec *)heap_caps_malloc(chgBytes, caps);
  g_chgMask = (uint8_t *)heap_caps_malloc(chgBytes / sizeof(Rec), caps);

  if (!g_raw || !g_chg || !g_chgMask) {
    // Report it rather than silently recording nothing, which would look
    // identical to a quiet bus.
    Serial.println("recorder: allocation FAILED, recording disabled.");
    g_rawCap = g_chgCap = 0;
    return;
  }

  g_rawCap = rawBytes / sizeof(Rec);
  g_chgCap = chgBytes / sizeof(Rec);

  Serial.printf("recorder: %s | raw ring %u frames (%u KB) | change log %u entries (%u KB)\n",
                g_psram ? "PSRAM" : "internal RAM (no PSRAM!)",
                (unsigned)g_rawCap, (unsigned)(rawBytes / 1024),
                (unsigned)g_chgCap, (unsigned)(chgBytes / 1024));
}

bool   recorderHasPsram()        { return g_psram; }
size_t recorderRawCapacity()     { return g_rawCap; }
size_t recorderChangeCapacity()  { return g_chgCap; }
bool   recorderRunning()         { return g_running; }
uint32_t recorderRawTotal()      { return g_rawTotal; }
uint32_t recorderChangeStored()  { return g_chgCount; }
uint32_t recorderChangeDropped() { return g_chgDrop; }

uint32_t recorderRawStored() {
  return (g_rawTotal < g_rawCap) ? g_rawTotal : (uint32_t)g_rawCap;
}

void recorderStart() { g_running = true; }
void recorderStop()  { g_running = false; }

void recorderFreezeRaw(bool freeze) { g_frozen = freeze; }
bool recorderRawFrozen()            { return g_frozen; }

void recorderClear() {
  lockRec();
  g_chgCount = 0;
  g_chgDrop  = 0;
  unlockRec();
}

static inline void fill(Rec &r, uint32_t id, bool extd, uint8_t dlc,
                        const uint8_t *data) {
  r.ms    = millis();
  r.id    = (uint16_t)id;
  r.dlc   = dlc;
  r.flags = extd ? 1 : 0;
  memset(r.data, 0, 8);
  memcpy(r.data, data, dlc > 8 ? 8 : dlc);
}

void recorderNoteRaw(const twai_message_t &msg) {
  if (!g_rawCap || g_frozen || msg.rtr) return;

  lockRec();
  fill(g_raw[g_rawHead], msg.identifier, msg.extd, msg.data_length_code, msg.data);
  g_rawHead = (g_rawHead + 1) % g_rawCap;
  g_rawTotal++;
  unlockRec();
}

void recorderNoteChange(uint32_t id, bool extd, uint8_t dlc,
                        const uint8_t *data, uint8_t changed) {
  if (!g_running || !g_chgCap) return;

  lockRec();
  if (g_chgCount >= g_chgCap) {
    // Stop rather than wrap. A change log that silently ate its own beginning
    // would make a session look shorter than it was.
    g_chgDrop++;
  } else {
    fill(g_chg[g_chgCount], id, extd, dlc, data);
    g_chgMask[g_chgCount] = changed;
    g_chgCount++;
  }
  unlockRec();
}

// ---------------------------------------------------------------------------
// CSV
// ---------------------------------------------------------------------------

static size_t emitRow(char *out, size_t cap, const Rec &r, int maskOrMinus1) {
  size_t n = 0;
  n += snprintf(out + n, cap - n, "%lu,%03X,%u,%u",
                (unsigned long)r.ms, r.id, r.flags & 1u, r.dlc);
  if (maskOrMinus1 >= 0) {
    n += snprintf(out + n, cap - n, ",%02X", (unsigned)maskOrMinus1);
  }
  // Always eight columns so the CSV is rectangular; bytes past dlc stay blank
  // rather than being written as 00, which would be indistinguishable from a
  // real zero byte.
  for (uint8_t i = 0; i < 8; i++) {
    if (i < r.dlc) n += snprintf(out + n, cap - n, ",%02X", r.data[i]);
    else           n += snprintf(out + n, cap - n, ",");
  }
  n += snprintf(out + n, cap - n, "\n");
  return n;
}

size_t recorderRawCsvChunk(char *out, size_t cap, RecCsvCursor *cur) {
  if (!g_rawCap) return 0;

  lockRec();
  const uint32_t stored = recorderRawStored();
  // Oldest first. Once the ring has wrapped the oldest entry sits at the head.
  const uint32_t base = (g_rawTotal <= g_rawCap) ? 0 : g_rawHead;

  size_t n = 0;
  if (!cur->headerDone) {
    cur->headerDone = true;
    n += snprintf(out + n, cap - n, "ms,id,ext,dlc,d0,d1,d2,d3,d4,d5,d6,d7\n");
  }

  while (cur->row < stored && (cap - n) > 96) {
    const Rec &r = g_raw[(base + cur->row) % g_rawCap];
    n += emitRow(out + n, cap - n, r, -1);
    cur->row++;
  }
  unlockRec();
  return n;
}

size_t recorderChangeCsvChunk(char *out, size_t cap, RecCsvCursor *cur) {
  if (!g_chgCap) return 0;

  lockRec();
  size_t n = 0;
  if (!cur->headerDone) {
    cur->headerDone = true;
    n += snprintf(out + n, cap - n,
                  "ms,id,ext,dlc,changed,d0,d1,d2,d3,d4,d5,d6,d7\n");
  }

  while (cur->row < g_chgCount && (cap - n) > 96) {
    n += emitRow(out + n, cap - n, g_chg[cur->row], g_chgMask[cur->row]);
    cur->row++;
  }
  unlockRec();
  return n;
}

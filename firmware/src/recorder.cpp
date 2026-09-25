#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "recorder.h"
#include "spscring.h"
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

// ⭐ THE CHANGE LOG IS AN SPSC RING (spscring.h). canTask is the only producer
// and never waits: it does not take any lock on this path. The consumer side
// (filestore drain, changes.csv download, discard, clear) serialises among
// itself on g_chgLock, which canTask never touches. Discarding drained rows
// is advancing the tail -- no memmove.
static Rec     *g_chg      = nullptr;
static uint8_t *g_chgMask  = nullptr;
static size_t   g_chgCap   = 0;
static SpscRing g_chgRing;
static std::atomic<uint32_t> g_chgDrop{0};   // producer ++, clear resets

static bool g_psram   = false;
static bool g_running = false;
static bool g_frozen  = false;
static const char *g_chgStatus = "not started";

// Raw ring: overwritten in place by canTask, read by the raw.csv download
// (which freezes it first). Its own lock, and canTask only ever TRIES it: a
// reader holding it costs a counted frame, never a wait.
static SemaphoreHandle_t g_rawLock = nullptr;
static std::atomic<uint32_t> g_rawBusy{0};
static inline void lockRaw()   { if (g_rawLock) xSemaphoreTake(g_rawLock, portMAX_DELAY); }
static inline void unlockRaw() { if (g_rawLock) xSemaphoreGive(g_rawLock); }

static SemaphoreHandle_t g_chgLock = nullptr;   // consumer side only
static inline void lockChg()   { if (g_chgLock) xSemaphoreTake(g_chgLock, portMAX_DELAY); }
static inline void unlockChg() { if (g_chgLock) xSemaphoreGive(g_chgLock); }

// ---------------------------------------------------------------------------

void recorderBegin() {
  if (!g_rawLock) g_rawLock = xSemaphoreCreateMutex();
  if (!g_chgLock) g_chgLock = xSemaphoreCreateMutex();

  g_psram = (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0);

  const size_t rawBytes = g_psram ? REC_RAW_BYTES_PSRAM : REC_RAW_BYTES_FALLBACK;
  const uint32_t caps = g_psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_8BIT;
  g_raw = (Rec *)heap_caps_malloc(rawBytes, caps);
  g_rawCap = g_raw ? rawBytes / sizeof(Rec) : 0;

  // ⭐ NO PSRAM, NO CHANGE LOG -- LOUDLY. The internal-RAM fallback was a
  // 32 KB log: ~1.4 s at 1,500 frames/s, so any filestore stall dropped rows.
  // A logger that quietly keeps a log it cannot sustain is worse than one
  // that says it is not logging. Tier B snapshots (the sniffer table, no
  // PSRAM needed) carry on; /api/v1/session reports "recorder".
  spscInit(&g_chgRing, 0);
  if (!g_psram) {
    g_chgStatus = "disabled: needs PSRAM";
    Serial.println("\n*** recorder: change log needs PSRAM, logging disabled "
                   "(Tier A change log OFF; Tier B snapshots continue). This "
                   "board has no PSRAM. ***\n");
  } else {
    const uint32_t rows = spscPow2Floor(REC_CHG_BYTES_PSRAM / sizeof(Rec));
    g_chg = (Rec *)heap_caps_malloc((size_t)rows * sizeof(Rec), caps);
    g_chgMask = (uint8_t *)heap_caps_malloc(rows, caps);
    if (g_chg && g_chgMask) {
      g_chgCap = rows;
      spscInit(&g_chgRing, rows);
      g_chgStatus = "ok";
    } else {
      g_chgStatus = "disabled: allocation failed";
      Serial.println("\n*** recorder: change log allocation FAILED, logging "
                     "disabled (Tier A change log OFF). ***\n");
    }
  }
  if (!g_raw) {
    // Report it rather than silently recording nothing, which would look
    // identical to a quiet bus.
    Serial.println("recorder: raw ring allocation FAILED, ring disabled.");
  }

  Serial.printf("recorder: %s | raw ring %u frames (%u KB) | change log %u "
                "entries (%u KB, %s)\n",
                g_psram ? "PSRAM" : "internal RAM (no PSRAM!)",
                (unsigned)g_rawCap, (unsigned)(g_rawCap * sizeof(Rec) / 1024),
                (unsigned)g_chgCap, (unsigned)(g_chgCap * sizeof(Rec) / 1024),
                g_chgStatus);
}

const char *recorderChangeLogStatus() { return g_chgStatus; }
uint32_t recorderRawBusyDropped() { return g_rawBusy.load(std::memory_order_relaxed); }

bool   recorderHasPsram()        { return g_psram; }
size_t recorderRawCapacity()     { return g_rawCap; }
size_t recorderChangeCapacity()  { return g_chgCap; }
bool   recorderRunning()         { return g_running; }
uint32_t recorderRawTotal()      { return g_rawTotal; }
uint32_t recorderChangeStored()  { return spscCount(&g_chgRing); }
uint32_t recorderChangeDropped() { return g_chgDrop.load(std::memory_order_relaxed); }

uint32_t recorderRawStored() {
  return (g_rawTotal < g_rawCap) ? g_rawTotal : (uint32_t)g_rawCap;
}

void recorderStart() { g_running = true; }
void recorderStop()  { g_running = false; }

void recorderFreezeRaw(bool freeze) { g_frozen = freeze; }
bool recorderRawFrozen()            { return g_frozen; }

void recorderClear() {
  lockChg();
  spscConsume(&g_chgRing, spscCount(&g_chgRing));
  g_chgDrop.store(0, std::memory_order_relaxed);
  unlockChg();
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

  // canTask: TRY, never wait. Only a raw.csv download holds this, and it
  // freezes the ring first, so in practice this does not fail.
  if (g_rawLock && xSemaphoreTake(g_rawLock, 0) != pdTRUE) {
    g_rawBusy.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  fill(g_raw[g_rawHead], msg.identifier, msg.extd, msg.data_length_code, msg.data);
  g_rawHead = (g_rawHead + 1) % g_rawCap;
  g_rawTotal++;
  unlockRaw();
}

void recorderNoteChange(uint32_t id, bool extd, uint8_t dlc,
                        const uint8_t *data, uint8_t changed) {
  if (!g_running) return;

  // canTask. No lock: spscring.h. A full ring refuses rather than wrapping --
  // a log that silently ate its own oldest rows would make a session look
  // shorter than it was -- and the refusal is counted.
  uint32_t slot;
  if (!spscReserve(&g_chgRing, &slot)) {
    if (g_chgCap) g_chgDrop.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  fill(g_chg[slot], id, extd, dlc, data);
  g_chgMask[slot] = changed;
  spscPublish(&g_chgRing);
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

  lockRaw();
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
  unlockRaw();
  return n;
}

size_t recorderChangeCsvChunk(char *out, size_t cap, RecCsvCursor *cur) {
  if (!g_chgCap) return 0;

  lockChg();
  size_t n = 0;
  if (!cur->headerDone) {
    cur->headerDone = true;
    n += snprintf(out + n, cap - n,
                  "ms,id,ext,dlc,changed,d0,d1,d2,d3,d4,d5,d6,d7\n");
  }

  // Rows [tail, head) are the producer's to leave alone until the tail moves,
  // and only this side moves it -- so they are stable while g_chgLock is held.
  const uint32_t stored = spscCount(&g_chgRing);
  while (cur->row < stored && (cap - n) > 96) {
    const uint32_t slot = spscSlot(&g_chgRing, cur->row);
    n += emitRow(out + n, cap - n, g_chg[slot], g_chgMask[slot]);
    cur->row++;
  }
  unlockChg();
  return n;
}

uint32_t recorderChangeDiscardThrough(uint32_t rows) {
  if (!g_chgCap || !rows) return 0;

  // O(1): the ring's tail advances. The old memmove of up to 1.5 MB of PSRAM
  // ran under the lock canTask needed; there is no such lock any more.
  lockChg();
  rows = spscConsume(&g_chgRing, rows);
  unlockChg();
  return rows;
}

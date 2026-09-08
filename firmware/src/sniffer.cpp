#include <Arduino.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sniffer.h"
#include "recorder.h"
#include "config.h"

// ---------------------------------------------------------------------------
// One row per CAN ID.
//
// byteChanges[] counts how often each byte differed from the IMMEDIATELY
// PRECEDING frame of the same ID. That is what separates a signal from a
// heartbeat: the 2012 Civic capture showed most IDs carry a rolling counter
// plus a checksum in the last byte (0x188: ..33 / ..06 / ..15 / ..24 -- high
// nibble counting up, low nibble counting down), and 0x255 carries a counter
// and its complement in bytes 6 and 7. A byte that changes on nearly every
// frame is a heartbeat. Without suppressing those, every row shows as changed
// and the table is worthless.
//
// changedMask is the separate, sticky question: does this byte differ from the
// baseline captured at the last clear?
// ---------------------------------------------------------------------------

struct IdSlot {
  uint32_t id;
  uint32_t count;
  uint32_t firstMs;
  uint32_t lastMs;
  uint32_t byteChanges[8];
  uint8_t  last[8];
  uint8_t  base[8];
  uint8_t  dlc;
  uint8_t  changedMask;
  bool     extd;
};

static IdSlot   g_slots[SNIFF_MAX_IDS];
static uint16_t g_count      = 0;
static uint16_t g_overflow   = 0;
static uint32_t g_markedAtMs = 0;

// The table is written by the CAN task and read by the web server on another
// core. Sections are tiny -- a scan of at most 64 slots -- so a plain mutex is
// the right tool. Taking it ~1000x/s is not measurable next to the bus itself.
static SemaphoreHandle_t g_lock = nullptr;

static inline void lockTable()   { if (g_lock) xSemaphoreTake(g_lock, portMAX_DELAY); }
static inline void unlockTable() { if (g_lock) xSemaphoreGive(g_lock); }

// Defined below; needed by snifferNote to decide what counts as a real change.
static bool isHeartbeatByte(const struct IdSlot &s, uint8_t i);

void snifferBegin() {
  if (!g_lock) g_lock = xSemaphoreCreateMutex();
}

uint16_t snifferIdCount()  { return g_count; }
uint16_t snifferOverflow() { return g_overflow; }

void snifferReset() {
  lockTable();
  memset(g_slots, 0, sizeof(g_slots));
  g_count      = 0;
  g_overflow   = 0;
  g_markedAtMs = millis();
  unlockTable();
}

void snifferClearMarks() {
  lockTable();
  for (uint16_t i = 0; i < g_count; i++) {
    memcpy(g_slots[i].base, g_slots[i].last, 8);
    g_slots[i].changedMask = 0;
  }
  g_markedAtMs = millis();
  unlockTable();
}

void snifferNote(const twai_message_t &msg) {
  if (msg.rtr) return;   // remote frames carry no payload to diff

  uint8_t interesting = 0;

  lockTable();

  IdSlot *s = nullptr;
  for (uint16_t i = 0; i < g_count; i++) {
    if (g_slots[i].id == msg.identifier && g_slots[i].extd == (bool)msg.extd) {
      s = &g_slots[i];
      break;
    }
  }

  if (!s) {
    // Overflow is counted, never silently ignored -- a table that quietly
    // stopped tracking IDs would look identical to a bus that has few.
    if (g_count >= SNIFF_MAX_IDS) { g_overflow++; unlockTable(); return; }
    s = &g_slots[g_count++];
    memset(s, 0, sizeof(*s));
    s->id      = msg.identifier;
    s->extd    = msg.extd;
    s->firstMs = millis();
    memcpy(s->last, msg.data, msg.data_length_code);
    memcpy(s->base, msg.data, msg.data_length_code);

    // First sight of an ID is a change from nothing, so the change log opens
    // with a full picture of the bus instead of starting mid-stream.
    interesting = (msg.data_length_code >= 8)
                      ? 0xFF
                      : (uint8_t)((1u << msg.data_length_code) - 1u);
  } else {
    uint8_t delta = 0;
    for (uint8_t i = 0; i < msg.data_length_code; i++) {
      if (msg.data[i] != s->last[i]) {
        s->byteChanges[i]++;
        delta |= (uint8_t)(1u << i);
      }
      if (msg.data[i] != s->base[i]) s->changedMask |= (uint8_t)(1u << i);
    }
    memcpy(s->last, msg.data, msg.data_length_code);

    // Rolling counters and checksums move on nearly every frame. Logging those
    // would defeat the entire point of a change log, so they are masked out.
    // Before SNIFF_MIN_SAMPLES there is no verdict yet and a little noise gets
    // through; it is bounded and self-correcting.
    uint8_t hb = 0;
    for (uint8_t i = 0; i < msg.data_length_code; i++) {
      if (isHeartbeatByte(*s, i)) hb |= (uint8_t)(1u << i);
    }
    interesting = (uint8_t)(delta & ~hb);
  }

  s->dlc    = msg.data_length_code;
  s->lastMs = millis();
  s->count++;

  unlockTable();

  // Deliberately outside the table lock: the recorder takes its own, and never
  // nesting the two removes the possibility of a lock-order bug later.
  if (interesting) {
    recorderNoteChange(msg.identifier, msg.extd, msg.data_length_code,
                       msg.data, interesting);
  }
}

// A byte counts as a heartbeat once there is enough evidence. Below
// SNIFF_MIN_SAMPLES frames every byte looks volatile, so judge nothing yet.
static bool isHeartbeatByte(const struct IdSlot &s, uint8_t i) {
  if (s.count < SNIFF_MIN_SAMPLES) return false;
  const uint32_t comparisons = s.count - 1;
  return (s.byteChanges[i] * 100UL) / comparisons >= SNIFF_HEARTBEAT_PCT;
}

void snifferPrint(uint32_t frames, uint32_t missed, uint32_t busErr) {
  lockTable();

  // Sort indices by ID for a stable, readable table. Done at print time (twice
  // a second) rather than on insert, so the RX path stays a plain scan.
  uint16_t idx[SNIFF_MAX_IDS];
  for (uint16_t i = 0; i < g_count; i++) idx[i] = i;
  for (uint16_t i = 1; i < g_count; i++) {
    const uint16_t key = idx[i];
    int16_t j = (int16_t)i - 1;
    while (j >= 0 && g_slots[idx[j]].id > g_slots[key].id) {
      idx[j + 1] = idx[j];
      j--;
    }
    idx[j + 1] = key;
  }

  Serial.printf("=== sniff | %u ids | %lu frames | missed %lu | bus_err %lu | "
                "marked %lus ago",
                g_count,
                (unsigned long)frames,
                (unsigned long)missed,
                (unsigned long)busErr,
                (unsigned long)((millis() - g_markedAtMs) / 1000));
  if (g_overflow) Serial.printf(" | OVERFLOW %u", g_overflow);
  Serial.println(" ===");
  Serial.println(" ID     per  count   data   ([xx] moved since clear, .. heartbeat)");

  for (uint16_t n = 0; n < g_count; n++) {
    const IdSlot &s = g_slots[idx[n]];

    const uint32_t period =
        (s.count > 1) ? (s.lastMs - s.firstMs) / (s.count - 1) : 0;

    Serial.printf("%s%03lX %4lu %6lu  ",
                  s.extd ? "x" : "0x",
                  (unsigned long)s.id,
                  (unsigned long)period,
                  (unsigned long)s.count);

    for (uint8_t i = 0; i < s.dlc; i++) {
      if (isHeartbeatByte(s, i)) {
        Serial.print(" .. ");
      } else if (s.changedMask & (1u << i)) {
        Serial.printf("[%02X]", s.last[i]);
      } else {
        Serial.printf(" %02X ", s.last[i]);
      }
    }
    Serial.println();
  }
  Serial.println();

  unlockTable();
}

// ---------------------------------------------------------------------------
// JSON snapshot for the web UI
// ---------------------------------------------------------------------------

size_t snifferSnapshotJson(char *out, size_t cap,
                           uint32_t frames, uint32_t missed, uint32_t busErr) {
  lockTable();

  uint16_t idx[SNIFF_MAX_IDS];
  for (uint16_t i = 0; i < g_count; i++) idx[i] = i;
  for (uint16_t i = 1; i < g_count; i++) {
    const uint16_t key = idx[i];
    int16_t j = (int16_t)i - 1;
    while (j >= 0 && g_slots[idx[j]].id > g_slots[key].id) {
      idx[j + 1] = idx[j];
      j--;
    }
    idx[j + 1] = key;
  }

  size_t n = 0;
  n += snprintf(out + n, cap - n,
                "{\"frames\":%lu,\"missed\":%lu,\"busErr\":%lu,"
                "\"ids\":%u,\"overflow\":%u,\"markedAgo\":%lu,\"rows\":[",
                (unsigned long)frames, (unsigned long)missed,
                (unsigned long)busErr, g_count, g_overflow,
                (unsigned long)((millis() - g_markedAtMs) / 1000));

  for (uint16_t k = 0; k < g_count; k++) {
    const IdSlot &s = g_slots[idx[k]];

    // Stop cleanly rather than emit half a row. A truncated snapshot would
    // parse as broken JSON and blank the page for no visible reason.
    if (cap - n < 160) break;

    const uint32_t period =
        (s.count > 1) ? (s.lastMs - s.firstMs) / (s.count - 1) : 0;

    uint8_t hb = 0;
    for (uint8_t i = 0; i < s.dlc; i++) {
      if (isHeartbeatByte(s, i)) hb |= (uint8_t)(1u << i);
    }

    n += snprintf(out + n, cap - n,
                  "%s{\"id\":%lu,\"ext\":%u,\"per\":%lu,\"n\":%lu,\"c\":%u,\"h\":%u,\"b\":[",
                  k ? "," : "",
                  (unsigned long)s.id, s.extd ? 1u : 0u,
                  (unsigned long)period, (unsigned long)s.count,
                  s.changedMask, hb);

    for (uint8_t i = 0; i < s.dlc; i++) {
      n += snprintf(out + n, cap - n, "%s%u", i ? "," : "", s.last[i]);
    }
    n += snprintf(out + n, cap - n, "]}");
  }

  n += snprintf(out + n, cap - n, "]}");

  unlockTable();
  return n;
}

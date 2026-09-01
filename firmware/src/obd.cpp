// cardiag — OBD-II request/response layer (Phase 1a)
//
// DRAFT, 2026-08-26. Never compiled against hardware, never run against a car.
// See obd.h for scope and for what is deliberately left out.

#include <Arduino.h>
#include <string.h>

#include "driver/twai.h"
#include "config.h"
#include "obd.h"

// The whole module is mode-gated so the Phase 0 binaries are unchanged by its
// existence -- SELFTEST and LISTEN builds compile this file to nothing.
#if CARDIAG_MODE == MODE_POLL

// ---------------------------------------------------------------------------
// PID decoders
//
// Formulas are the published SAE J1979 ones. `d[0]` is "A", `d[1]` is "B",
// which is how every reference writes them — keeping that naming makes the
// table checkable against any PID reference without mental translation.
// ---------------------------------------------------------------------------

static float dec_percent255(const uint8_t *d) { return d[0] * 100.0f / 255.0f; }
static float dec_tempMinus40(const uint8_t *d) { return (float)d[0] - 40.0f; }
static float dec_trim(const uint8_t *d) { return (d[0] - 128) * 100.0f / 128.0f; }
static float dec_raw(const uint8_t *d) { return (float)d[0]; }
static float dec_rpm(const uint8_t *d) { return ((d[0] * 256.0f) + d[1]) / 4.0f; }
static float dec_timingAdv(const uint8_t *d) { return (d[0] / 2.0f) - 64.0f; }
static float dec_moduleVolts(const uint8_t *d) { return ((d[0] * 256.0f) + d[1]) / 1000.0f; }

// Tier-B starter set. Small on purpose: these are the signals the analysis
// layer actually needs (the thermostat case wants coolant + trims + load), and
// a short list keeps the first sweep fast enough to reason about.
const ObdPid OBD_PID_TABLE[] = {
  { 0x04, "LOAD",     "%",    1, dec_percent255  },
  { 0x05, "COOLANT",  "C",    1, dec_tempMinus40 },
  { 0x06, "STFT",     "%",    1, dec_trim        },
  { 0x07, "LTFT",     "%",    1, dec_trim        },
  { 0x0B, "MAP",      "kPa",  1, dec_raw         },
  { 0x0C, "RPM",      "rpm",  2, dec_rpm         },
  { 0x0D, "SPEED",    "km/h", 1, dec_raw         },
  { 0x0E, "TIMING",   "deg",  1, dec_timingAdv   },
  { 0x0F, "IAT",      "C",    1, dec_tempMinus40 },
  { 0x11, "THROTTLE", "%",    1, dec_percent255  },
  // System rail as the ECU sees it. Not a substitute for measuring at the
  // load: it shows system-level sag, not the drop across any one feed.
  { 0x42, "VOLTS",    "V",    2, dec_moduleVolts },
};
const size_t OBD_PID_TABLE_LEN = sizeof(OBD_PID_TABLE) / sizeof(OBD_PID_TABLE[0]);

const ObdPid *obdFindPid(uint8_t pid) {
  for (size_t i = 0; i < OBD_PID_TABLE_LEN; i++) {
    if (OBD_PID_TABLE[i].pid == pid) return &OBD_PID_TABLE[i];
  }
  return nullptr;
}

// ---------------------------------------------------------------------------

static ObdStats g_stats = {};

const ObdStats *obdStats(void) { return &g_stats; }
void obdResetStats(void) { memset(&g_stats, 0, sizeof(g_stats)); }

// Throw away anything already queued. Without this, a stale reply from the
// previous request gets matched against the current one and every reading is
// silently one request out of date — the kind of bug that produces plausible
// numbers, which is the worst kind to have in a dataset.
static void drainRx(void) {
  twai_message_t junk;
  while (twai_receive(&junk, 0) == ESP_OK) { /* discard */ }
}

// ---------------------------------------------------------------------------
// One request, one reply.
//
// Request frame (ISO-TP single frame):
//   [0] PCI  = number of meaningful bytes that follow (0x02 = mode + pid)
//   [1] mode
//   [2] pid
//   [3..7] padding
//
// Reply frame:
//   [0] PCI  = 0x0N, N = bytes that follow (mode echo + pid echo + payload)
//   [1] mode + 0x40
//   [2] pid echo
//   [3..] payload
// ---------------------------------------------------------------------------

bool obdRequest(uint8_t mode, uint8_t pid, ObdResult *out) {
  memset(out, 0, sizeof(*out));
  g_stats.requests++;

  drainRx();

  twai_message_t tx = {};
  tx.identifier       = OBD_REQ_ID_FUNCTIONAL;
  tx.data_length_code = 8;
  tx.data[0] = 0x02;
  tx.data[1] = mode;
  tx.data[2] = pid;
  // 0x55 padding is conventional and makes request frames obvious in a capture.
  tx.data[3] = tx.data[4] = tx.data[5] = tx.data[6] = tx.data[7] = 0x55;

  const uint32_t t0 = millis();

  if (twai_transmit(&tx, pdMS_TO_TICKS(OBD_RESPONSE_TIMEOUT_MS)) != ESP_OK) {
    // Not the same failure as a timeout: this means we never got the frame
    // onto the bus at all (no ACK from any node => nobody is listening).
    g_stats.timeouts++;
    return false;
  }

  const uint8_t wantMode = mode + OBD_RESPONSE_OFFSET;

  while ((millis() - t0) < OBD_RESPONSE_TIMEOUT_MS) {
    twai_message_t rx;
    if (twai_receive(&rx, pdMS_TO_TICKS(5)) != ESP_OK) continue;

    if (rx.extd) continue;
    if (rx.identifier < OBD_RESP_ID_FIRST || rx.identifier > OBD_RESP_ID_LAST) continue;

    // Several ECUs can answer a broadcast request. Track who does — a PID that
    // only ever comes back from one address tells you where it lives.
    g_stats.respondersMask |= (uint16_t)(1u << (rx.identifier - OBD_RESP_ID_FIRST));

    const uint8_t pci = rx.data[0];

    // First Frame of a multi-frame reply. Decoding it needs flow control,
    // which is Mode 06 / VIN territory and out of scope here. Report it rather
    // than pretending it did not happen.
    if ((pci & 0xF0) == 0x10) {
      g_stats.multiframe++;
      out->multiframe = true;
      out->respId     = rx.identifier;
      return false;
    }

    if ((pci & 0xF0) != 0x00) continue;      // not a single frame
    if (pci < 2 || pci > 7) { g_stats.malformed++; continue; }
    if (rx.data[1] != wantMode) continue;    // a reply, but not to our mode
    if (rx.data[2] != pid)     continue;     // a reply, but not to our PID

    out->len = pci - 2;
    if (out->len > sizeof(out->data)) { g_stats.malformed++; return false; }
    memcpy(out->data, &rx.data[3], out->len);

    out->ok        = true;
    out->respId    = rx.identifier;
    out->elapsedMs = millis() - t0;

    g_stats.replies++;
    g_stats.lastLatencyMs = out->elapsedMs;
    return true;
  }

  g_stats.timeouts++;
  return false;
}

// ---------------------------------------------------------------------------
// Supported-PID discovery
//
// PID 0x00 returns a 4-byte bitmap for PIDs 0x01-0x20, 0x20 covers 0x21-0x40,
// 0x40 covers 0x41-0x60. Bit 7 of byte A is the LOWEST PID in each block, so
// the natural packing is big-endian: PID n lands at bit (32 - n) of its word.
//
// The top bit of each block (0x20, 0x40) means "the next block exists", which
// is why this walks rather than querying all three unconditionally.
// ---------------------------------------------------------------------------

bool obdPidSupported(const uint32_t *bitmap, uint8_t pid) {
  if (pid == 0 || pid > OBD_MAX_PID) return false;
  const uint8_t n    = pid - 1;
  const uint8_t word = n / 32;
  const uint8_t bit  = 31 - (n % 32);
  return (bitmap[word] >> bit) & 1u;
}

uint8_t obdDiscoverSupported(uint32_t *bitmap) {
  memset(bitmap, 0, OBD_BITMAP_WORDS * sizeof(uint32_t));

  const uint8_t blockPids[OBD_BITMAP_WORDS] = { 0x00, 0x20, 0x40 };

  for (uint8_t b = 0; b < OBD_BITMAP_WORDS; b++) {
    ObdResult r;
    if (!obdRequest(OBD_MODE_CURRENT_DATA, blockPids[b], &r)) break;
    if (r.len < 4) break;

    bitmap[b] = ((uint32_t)r.data[0] << 24) | ((uint32_t)r.data[1] << 16) |
                ((uint32_t)r.data[2] << 8)  | (uint32_t)r.data[3];

    // Bit 0 of the block = "next block supported". Stop if it is clear.
    if (!(bitmap[b] & 0x00000001u)) break;

    delay(OBD_INTER_REQUEST_MS);
  }

  uint8_t count = 0;
  for (uint8_t pid = 1; pid <= OBD_MAX_PID; pid++) {
    if (obdPidSupported(bitmap, pid)) count++;
  }
  return count;
}

#endif  // CARDIAG_MODE == MODE_POLL

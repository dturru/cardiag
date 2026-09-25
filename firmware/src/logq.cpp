#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"

#include "logq.h"
#include "config.h"

// NOSPLIT: an item is one whole line, received whole, so a line can never be
// written out in two pieces.
static RingbufHandle_t g_rb = nullptr;
static std::atomic<uint32_t> g_dropped{0};

static int logqVprintf(const char *fmt, va_list ap) {
  char buf[LOGQ_LINE_MAX];
  const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  if (n <= 0) return n;
  const size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
  logqPush(buf, len);
  return n;
}

void logqBegin() {
  if (g_rb) return;
  g_rb = xRingbufferCreate(LOGQ_BYTES, RINGBUF_TYPE_NOSPLIT);
  // ESP-IDF logs from whichever task raised them (the Wi-Fi driver's, the
  // event loop's). Queue those too, or they interleave with loop()'s lines.
  if (g_rb) esp_log_set_vprintf(logqVprintf);
}

void logqPush(const char *line, size_t len) {
  if (!len) return;
  if (!g_rb || xRingbufferSend(g_rb, line, len, 0) != pdTRUE) {
    g_dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

void logqPrintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  logqVprintf(fmt, ap);
  va_end(ap);
}

void logqDrain(size_t budgetBytes) {
  if (!g_rb) return;
  size_t done = 0;
  while (done < budgetBytes) {
    size_t n = 0;
    char *item = (char *)xRingbufferReceive(g_rb, &n, 0);
    if (!item) break;
    Serial.write((const uint8_t *)item, n);
    vRingbufferReturnItem(g_rb, item);
    done += n;
  }
}

uint32_t logqDropped() { return g_dropped.load(std::memory_order_relaxed); }

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "heapdiag.h"
#include "hubresolve.h"
#include "config.h"
#include "session.h"

static HeapSample     g_s = {0, 0, 0, 0, 0, -1, -1};
static HeapCheckState g_c = {};
static uint32_t       g_lastCheckMs = 0;
static uint32_t       g_lastAllMs = 0;

// Survives a software reset / panic (not a power cut). The magic says the
// rest is ours and not power-on garbage.
struct HeapFailRecord {
  uint32_t magic;
  uint32_t bootId, failMs, lastOkMs;
  char     mdns[48];
};
static const uint32_t HEAPFAIL_MAGIC = 0x48504631;   // "HPF1"
RTC_NOINIT_ATTR static HeapFailRecord s_rec;

static void sample(uint32_t now) {
  g_s.atMs = now ? now : 1;
  g_s.freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  g_s.minFreeInternal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  g_s.largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  g_s.largest8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  // The mdns component runs its own task on a CONFIG_MDNS_TASK_STACK_SIZE
  // (4096) stack allocated from the internal heap: an overflow that jumps
  // the end-of-stack watchpoint lands in a neighbouring heap block.
  TaskHandle_t t = xTaskGetHandle("mdns");
  if (t) {
    g_s.mdnsStackFree = (int32_t)uxTaskGetStackHighWaterMark(t);
    if (g_s.mdnsStackFreeMin < 0 || g_s.mdnsStackFree < g_s.mdnsStackFreeMin)
      g_s.mdnsStackFreeMin = g_s.mdnsStackFree;
  } else {
    g_s.mdnsStackFree = -1;
  }
}

void heapdiagBegin() {
  g_c.enabled = CARDIAG_HEAP_CHECK != 0;
  if (s_rec.magic == HEAPFAIL_MAGIC) {
    s_rec.mdns[sizeof(s_rec.mdns) - 1] = '\0';
    g_c.prevValid = true;
    g_c.prevBootId = s_rec.bootId;
    g_c.prevFailMs = s_rec.failMs;
    g_c.prevLastOkMs = s_rec.lastOkMs;
    memcpy(g_c.prevMdns, s_rec.mdns, sizeof(g_c.prevMdns));
    Serial.printf("[heap] PREVIOUS BOOT %lu: integrity check FAILED at %lu ms "
                  "(last ok %lu ms); mdns: %s\n",
                  (unsigned long)s_rec.bootId, (unsigned long)s_rec.failMs,
                  (unsigned long)s_rec.lastOkMs, s_rec.mdns);
    s_rec.magic = 0;              // reported once
  }
  sample(millis());
  Serial.printf("[heap] integrity check %s (every %u ms), mdns resolve=%d advertise=%d\n",
                g_c.enabled ? "ON" : "off", (unsigned)HEAP_CHECK_PERIOD_MS,
                (int)CARDIAG_MDNS_RESOLVE, (int)CARDIAG_MDNS_ADVERTISE);
}

#if CARDIAG_HEAP_CHECK
static void check(uint32_t now, bool all) {
  const uint32_t t0 = micros();
  // print_errors=true: the heap code prints the corrupt block's address and
  // what it expected there, before we print our context line.
  const bool ok = all ? heap_caps_check_integrity_all(true)
                      : heap_caps_check_integrity(MALLOC_CAP_INTERNAL, true);
  const uint32_t us = micros() - t0;
  g_c.checks++;
  if (us > g_c.maxCheckUs) g_c.maxCheckUs = us;
  if (ok) {
    g_c.lastOkMs = now;
    return;
  }
  g_c.failures++;
  if (g_c.firstFailMs) return;    // the first one is the one that matters
  g_c.firstFailMs = now;
  hubResolveDescribeRecent(g_c.firstFailMdns, sizeof(g_c.firstFailMdns), now);
  s_rec.magic = HEAPFAIL_MAGIC;
  s_rec.bootId = sessionBootId();
  s_rec.failMs = now;
  s_rec.lastOkMs = g_c.lastOkMs;
  memcpy(s_rec.mdns, g_c.firstFailMdns, sizeof(s_rec.mdns));
  Serial.printf("[heap] INTEGRITY CHECK FAILED (%s) boot=%lu t=%lu ms (check #%lu, last "
                "ok %lu ms, %lu ms ago); last mdns: %s; mdns stack free min %ld B\n",
                all ? "all heaps" : "internal",
                (unsigned long)sessionBootId(), (unsigned long)now,
                (unsigned long)g_c.checks, (unsigned long)g_c.lastOkMs,
                (unsigned long)(now - g_c.lastOkMs), g_c.firstFailMdns,
                (long)g_s.mdnsStackFreeMin);
}
#endif

void heapdiagLoop() {
  const uint32_t now = millis();
  if (!g_s.atMs || now - g_s.atMs >= HEAP_SAMPLE_MS) sample(now);
#if CARDIAG_HEAP_CHECK
  if (now - g_lastAllMs >= HEAP_CHECK_ALL_MS) {
    g_lastAllMs = now;
    g_lastCheckMs = now;
    check(now, true);
  } else if (now - g_lastCheckMs >= HEAP_CHECK_PERIOD_MS) {
    g_lastCheckMs = now;
    check(now, false);
  }
#else
  (void)g_lastCheckMs;
  (void)g_lastAllMs;
#endif
}

const HeapSample     *heapdiagSample() { return &g_s; }
const HeapCheckState *heapdiagCheck() { return &g_c; }

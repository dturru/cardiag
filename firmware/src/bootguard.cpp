#include <Arduino.h>
#include <Preferences.h>
#include <esp_attr.h>
#include <esp_app_desc.h>
#include <esp_task_wdt.h>

#include "bootguard.h"
#include "bootguard_rt.h"

// Survives a watchdog or panic reset; garbage after a power-on, which is what
// the magic and check word in BgRecord are for.
RTC_NOINIT_ATTR static BgRecord s_rtc;

static uint8_t s_prior = 0;

// The firmware identity a record belongs to: the first four bytes of the app
// ELF sha. A new build reads an old record as invalid, so flashing a fix
// starts the count from zero instead of booting straight back into safe mode.
static uint32_t appTag() {
  const esp_app_desc_t *d = esp_app_get_description();
  uint32_t tag = 0;
  memcpy(&tag, d->app_elf_sha256, sizeof(tag));
  return tag;
}

static bool nvsRead(BgRecord *out) {
  Preferences p;
  if (!p.begin("cardiagbg", true)) return false;
  const size_t n = p.getBytes("rec", out, sizeof(*out));
  p.end();
  return n == sizeof(*out);
}

static void nvsWrite(const BgRecord &r) {
  Preferences p;
  if (!p.begin("cardiagbg", false)) return;
  p.putBytes("rec", &r, sizeof(r));
  p.end();
}

uint8_t bootguardBegin() {
  const uint32_t tag = appTag();
  BgRecord nvs;
  const bool haveNvs = nvsRead(&nvs);
  s_prior = bgPriorAttempts(&s_rtc, haveNvs ? &nvs : nullptr, tag);
  Serial.printf("[boot] filestore start attempts unfinished: %u (safe mode at "
                "%u)%s\n", (unsigned)s_prior, (unsigned)BG_FAIL_LIMIT,
                bgShouldSafeMode(s_prior) ? "  *** SAFE MODE ***" : "");
  return s_prior;
}

void bootguardMarkStart(uint8_t prior) {
  // RTC first: it is a plain memory write and cannot fail. NVS second, so a
  // watchdog that fires during the NVS commit still leaves the RTC copy.
  const BgRecord r = bgMake(appTag(), bgOnStart(prior));
  s_rtc = r;
  nvsWrite(r);
}

void bootguardClear() {
  const BgRecord r = bgMake(appTag(), 0);
  s_rtc = r;
  nvsWrite(r);
}

uint8_t bootguardPriorAttempts() { return s_prior; }

void bootguardArmTaskWdt(uint32_t seconds) {
  esp_task_wdt_config_t wdt = {
      .timeout_ms = seconds * 1000u,
      .idle_core_mask = 0,
      .trigger_panic = true,   // reset, do not just complain
  };
  // Already initialised by the Arduino core on some builds, and by the first
  // call here on the second; reconfigure rather than treating "already
  // exists" as a failure.
  if (esp_task_wdt_init(&wdt) == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&wdt);
  }
  if (esp_task_wdt_status(nullptr) != ESP_OK) esp_task_wdt_add(nullptr);
  esp_task_wdt_reset();        // the new window starts now, not at the old one
}

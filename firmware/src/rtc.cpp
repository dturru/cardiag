#include <Arduino.h>
#include <Wire.h>

#include "rtc.h"
#include "rtc8563.h"
#include "session.h"
#include "config.h"

static bool g_present = false;

#if RTC_ENABLE
static bool readTime(uint8_t r[PCF8563_TIME_LEN]) {
  Wire.beginTransmission(PCF8563_I2C_ADDR);
  Wire.write(PCF8563_REG_TIME);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint16_t)PCF8563_I2C_ADDR, (size_t)PCF8563_TIME_LEN,
                       true) != PCF8563_TIME_LEN)
    return false;
  for (int i = 0; i < PCF8563_TIME_LEN; i++) r[i] = (uint8_t)Wire.read();
  return true;
}
#endif

void rtcBegin() {
#if !RTC_ENABLE
  Serial.println("[rtc] disabled in this build -- no rtc anchor");
#else
  if (!Wire.begin(RTC_SDA_GPIO, RTC_SCL_GPIO, RTC_I2C_HZ)) {
    Serial.println("[rtc] I2C init FAILED -- no rtc anchor");
    return;
  }
  uint8_t r[PCF8563_TIME_LEN];
  if (!readTime(r)) {
    Serial.printf("[rtc] no PCF8563 at 0x%02X (SDA %d, SCL %d) -- no rtc "
                  "anchor\n", PCF8563_I2C_ADDR, RTC_SDA_GPIO, RTC_SCL_GPIO);
    return;
  }
  g_present = true;
  int64_t s = 0;
  const RtcStatus st = rtcDecode(r, RTC_MIN_YEAR, &s);
  if (st != RTC_OK) {
    // Never invent time: no anchor until the hub pushes one.
    Serial.printf("[rtc] PCF8563 time NOT trusted (%s) -- no rtc anchor\n",
                  rtcStatusName(st));
    return;
  }
  sessionSetAnchor((uint64_t)s * 1000u, TIME_RTC);
  Serial.printf("[rtc] PCF8563 ok, VL clear -- anchor epoch_s=%lld "
                "(source rtc, lowest trust)\n", (long long)s);
#endif
}

void rtcSyncFromAnchor() {
#if RTC_ENABLE
  if (!g_present || !sessionAnchorValid() || sessionTimeSource() < TIME_NTP)
    return;
  const uint64_t ms = sessionAnchorEpochMs() +
                      (uint32_t)(millis() - sessionAnchorUptimeMs());
  uint8_t r[PCF8563_TIME_LEN];
  if (!rtcEncode((int64_t)(ms / 1000u), r)) {
    Serial.println("[rtc] hub time outside 2000-2099 -- RTC not written");
    return;
  }
  Wire.beginTransmission(PCF8563_I2C_ADDR);
  Wire.write(PCF8563_REG_TIME);
  Wire.write(r, PCF8563_TIME_LEN);
  const uint8_t e = Wire.endTransmission();
  Serial.printf("[rtc] %s from %s anchor\n", e == 0 ? "set" : "write FAILED",
                sessionTimeSourceName());
#endif
}

bool rtcPresent() { return g_present; }

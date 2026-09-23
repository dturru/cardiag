#include <Arduino.h>
#include <Preferences.h>
#include <esp_mac.h>

#include "session.h"
#include "config.h"

static Preferences g_ns;

static uint32_t g_bootId    = 0;
static uint32_t g_deviceId  = 0;

static bool              g_anchorValid  = false;
static uint64_t          g_anchorEpoch  = 0;
static uint32_t          g_anchorUptime = 0;
static SessionTimeSource g_timeSource   = TIME_NONE;

static uint32_t g_lastMillis = 0;

// A monotonic counter, not a random u32. Unique AND orderable, so the hub can
// sort sessions chronologically with no clock at all -- which matters because
// a session may never get an anchor.
static uint32_t nextBootId() {
  g_ns.begin("cardiag-hub", false);
  uint32_t n = g_ns.getUInt("boot", 0) + 1;
  g_ns.putUInt("boot", n);
  g_ns.end();
  return n;
}

void sessionBegin() {
  g_bootId = nextBootId();

  // Low 4 octets of the MAC: the OUI carries no per-device entropy. Must match
  // carhub's proto.device_id_from_mac(), which reads the same 4 bytes
  // little-endian.
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  g_deviceId = (uint32_t)mac[2] | ((uint32_t)mac[3] << 8) |
               ((uint32_t)mac[4] << 16) | ((uint32_t)mac[5] << 24);

  g_lastMillis = millis();

  Serial.printf("[session] boot_id=%lu device_id=0x%08lX\n",
                (unsigned long)g_bootId, (unsigned long)g_deviceId);
}

uint32_t sessionBootId()   { return g_bootId; }
uint32_t sessionDeviceId() { return g_deviceId; }

bool sessionSetAnchor(uint64_t epoch_ms, SessionTimeSource source) {
  if (source == TIME_NONE) return false;
  if (g_anchorValid && source < g_timeSource) {
    // Keep the more trustworthy anchor.
    return false;
  }
  g_anchorEpoch  = epoch_ms;
  g_anchorUptime = millis();
  g_timeSource   = source;
  g_anchorValid  = true;
  Serial.printf("[session] anchor set: epoch=%llu uptime=%lu source=%s\n",
                (unsigned long long)epoch_ms,
                (unsigned long)g_anchorUptime, sessionTimeSourceName());
  return true;
}

bool              sessionAnchorValid()     { return g_anchorValid; }
uint64_t          sessionAnchorEpochMs()   { return g_anchorEpoch; }
uint32_t          sessionAnchorUptimeMs()  { return g_anchorUptime; }
SessionTimeSource sessionTimeSource()      { return g_timeSource; }

const char *sessionTimeSourceName() {
  switch (g_timeSource) {
    case TIME_GPS: return "gps";
    case TIME_NTP: return "ntp";
    case TIME_RTC: return "rtc";
    default:       return "none";
  }
}

bool sessionTick() {
  const uint32_t now = millis();
  if (now >= g_lastMillis) { g_lastMillis = now; return false; }

  // millis() went backwards => the 32-bit counter wrapped (~49.7 days).
  // Close the session so relative time stays monotonic within a boot_id.
  g_bootId = nextBootId();
  g_anchorValid  = false;
  g_timeSource   = TIME_NONE;
  g_anchorEpoch  = 0;
  g_anchorUptime = 0;
  g_lastMillis   = now;
  Serial.printf("[session] millis() wrapped; new boot_id=%lu, anchor cleared\n",
                (unsigned long)g_bootId);
  return true;
}

uint32_t sessionSnapshotBytesPerSec(uint16_t idCount) {
  // Per-second block: u32 ms + u16 count + N x (u32 id + u8 dlc + u8 data[8])
  return 4u + 2u + (uint32_t)idCount * 13u;
}

uint32_t sessionSnapshotSeconds(uint16_t idCount) {
  const uint32_t rate = sessionSnapshotBytesPerSec(idCount);
  if (!rate) return 0;
  return (uint32_t)(HUB_FS_USABLE_BYTES / rate);
}

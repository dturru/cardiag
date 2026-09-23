#include <Arduino.h>
#include <WebServer.h>
#include <esp_heap_caps.h>

#include "hubapi.h"
#include "hubproto.h"
#include "hublink.h"
#include "hubstream.h"
#include "session.h"
#include "sniffer.h"
#include "config.h"
#include "secrets.h"

// Constant-time compare. A length-dependent early return on a shared token is
// a timing oracle; cheap to avoid, so avoid it.
static bool tokenOk(WebServer &srv) {
  if (!srv.hasHeader("X-Hub-Token")) return false;
  const String got = srv.header("X-Hub-Token");
  const char  *want = HUB_API_TOKEN;
  const size_t wlen = strlen(want);
  uint8_t diff = (uint8_t)(got.length() ^ wlen);
  for (size_t i = 0; i < wlen; i++) {
    const char c = (i < got.length()) ? got[i] : 0;
    diff |= (uint8_t)(c ^ want[i]);
  }
  return diff == 0;
}

static void denied(WebServer &srv) {
  srv.send(401, "application/json", "{\"error\":\"bad or missing X-Hub-Token\"}");
}

// GET /api/v1/session
static void handleSession(WebServer &srv) {
  const uint16_t ids = snifferIdCount();
  const uint32_t rate = sessionSnapshotBytesPerSec(ids);
  const uint32_t secs = sessionSnapshotSeconds(ids);

  char buf[768];
  int n = snprintf(buf, sizeof(buf),
      "{\"proto\":%d,"
      "\"device_id\":%lu,"
      "\"boot_id\":%lu,"
      "\"uptime_ms\":%lu,"
      "\"link\":\"%s\",",
      HUB_PROTO_VERSION,
      (unsigned long)sessionDeviceId(),
      (unsigned long)sessionBootId(),
      (unsigned long)millis(),
      hublinkStateName());

  if (sessionAnchorValid()) {
    n += snprintf(buf + n, sizeof(buf) - n,
        "\"anchor\":{\"epoch_ms\":%llu,\"uptime_ms\":%lu,\"source\":\"%s\"},",
        (unsigned long long)sessionAnchorEpochMs(),
        (unsigned long)sessionAnchorUptimeMs(),
        sessionTimeSourceName());
  } else {
    // Protocol 3.4: no anchor means null. Never a guessed timestamp.
    n += snprintf(buf + n, sizeof(buf) - n, "\"anchor\":null,");
  }

  // Retention is MEASURED, not assumed: distinct ids counted at runtime, so the
  // projection is right for this car without a code change -- and will be right
  // for the BMW too, which will have a different id count.
  n += snprintf(buf + n, sizeof(buf) - n,
      "\"snapshot\":{"
        "\"ids_observed\":%u,"
        "\"ids_max\":%u,"
        "\"overflow\":%u,"
        "\"bytes_per_sec\":%lu,"
        "\"projected_seconds\":%lu,"
        "\"fs_usable_bytes\":%llu,"
        "\"basis\":\"runtime id count; block = u32 ms + u16 count + N*13\"}}",
      (unsigned)ids, (unsigned)SNIFF_MAX_IDS, (unsigned)snifferOverflow(),
      (unsigned long)rate, (unsigned long)secs,
      (unsigned long long)HUB_FS_USABLE_BYTES);

  srv.send(200, "application/json", buf);
}

// POST /api/v1/time   {"epoch_ms": <u64>, "source": "gps"|"ntp"|"rtc"}
static void handleTime(WebServer &srv) {
  if (!tokenOk(srv)) { denied(srv); return; }

  const String body = srv.arg("plain");
  // Deliberately a minimal parse rather than pulling in a JSON library for two
  // fields. If this grows a third field, use ArduinoJson.
  long long epoch = -1;
  int k = body.indexOf("\"epoch_ms\"");
  if (k >= 0) {
    int c = body.indexOf(':', k);
    if (c >= 0) epoch = atoll(body.c_str() + c + 1);
  }
  SessionTimeSource src = TIME_NONE;
  if (body.indexOf("\"gps\"") >= 0)      src = TIME_GPS;
  else if (body.indexOf("\"ntp\"") >= 0) src = TIME_NTP;
  else if (body.indexOf("\"rtc\"") >= 0) src = TIME_RTC;

  if (epoch <= 0 || src == TIME_NONE) {
    srv.send(400, "application/json",
             "{\"error\":\"need epoch_ms and source in {gps,ntp,rtc}\"}");
    return;
  }

  const bool applied = sessionSetAnchor((uint64_t)epoch, src);
  char buf[192];
  snprintf(buf, sizeof(buf),
      "{\"ok\":true,\"applied\":%s,\"source\":\"%s\",\"uptime_ms\":%lu}",
      applied ? "true" : "false", sessionTimeSourceName(),
      (unsigned long)sessionAnchorUptimeMs());
  srv.send(200, "application/json", buf);
}

// GET /api/v1/files -- Phase B. Answered explicitly rather than 404 so the hub
// can tell "no file store yet" from "wrong URL".
static void handleFilesStub(WebServer &srv) {
  srv.send(501, "application/json",
      "{\"error\":\"no file store yet\","
      "\"detail\":\"Phase B. Needs LittleFS or the carrier board microSD.\","
      "\"see\":\"docs/hub-integration-plan.md\"}");
}

void hubapiRegister(WebServer &srv) {
  // WebServer only retains headers it was told to collect.
  const char *wanted[] = {"X-Hub-Token"};
  srv.collectHeaders(wanted, 1);

  srv.on("/api/v1/session", HTTP_GET,  [&srv]() { handleSession(srv); });
  srv.on("/api/v1/time",    HTTP_POST, [&srv]() { handleTime(srv); });
  srv.on("/api/v1/files",   HTTP_GET,  [&srv]() { handleFilesStub(srv); });
}

#include <Arduino.h>
#include <WebServer.h>
#include <esp_heap_caps.h>
#include <stdarg.h>

#include "hubapi.h"
#include "hubproto.h"
#include "hublink.h"
#include "hubstream.h"
#include "filestore.h"
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

// snprintf returns the length it WOULD have written, so chaining it by adding
// the return value walks the cursor past the buffer as soon as one field does
// not fit -- and the next call then gets a negative size, which converts to a
// huge size_t. This clamps instead: the JSON truncates, which a parser rejects
// loudly, rather than the stack being overwritten quietly.
static int jsonAppend(char *buf, size_t cap, int n, const char *fmt, ...) {
  if (n < 0 || (size_t)n >= cap) return (int)cap;
  va_list ap;
  va_start(ap, fmt);
  const int w = vsnprintf(buf + n, cap - (size_t)n, fmt, ap);
  va_end(ap);
  if (w < 0) return (int)cap;
  n += w;
  return ((size_t)n >= cap) ? (int)cap : n;
}

// GET /api/v1/session
static void handleSession(WebServer &srv) {
  const uint16_t ids = snifferIdCount();
  const uint32_t rate = sessionSnapshotBytesPerSec(ids);
  const uint32_t secs = sessionSnapshotSeconds(ids);

  char buf[2048];
  int n = jsonAppend(buf, sizeof(buf), 0,
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
    n = jsonAppend(buf, sizeof(buf), n,
        "\"anchor\":{\"epoch_ms\":%llu,\"uptime_ms\":%lu,\"source\":\"%s\"},",
        (unsigned long long)sessionAnchorEpochMs(),
        (unsigned long)sessionAnchorUptimeMs(),
        sessionTimeSourceName());
  } else {
    // Protocol 3.4: no anchor means null. Never a guessed timestamp.
    n = jsonAppend(buf, sizeof(buf), n, "\"anchor\":null,");
  }

  // Retention is MEASURED, not assumed: distinct ids counted at runtime, so the
  // projection is right for this car without a code change -- and will be right
  // for the BMW too, which will have a different id count.
  //
  // TWO projections, because they answer different questions and quoting only
  // the first would overstate retention by 2.5x:
  //   projected_seconds_nominal  the whole partition, ignoring Tier C
  //   projected_seconds          Tier B's GUARANTEED share -- what the device
  //                              actually promises to keep, since Tier C is
  //                              capped at FS_TIER_A_MAX_PCT and the rest is
  //                              Tier B's floor
  const FileStoreStats *fsb = filestoreStats();
  const uint64_t fsTotal = fsb->totalBytes ? fsb->totalBytes
                                           : (uint64_t)HUB_FS_USABLE_BYTES;
  const uint64_t tierBFloor = fsTotal * (100 - FS_TIER_A_MAX_PCT) / 100;
  n = jsonAppend(buf, sizeof(buf), n,
      "\"snapshot\":{"
        "\"ids_observed\":%u,"
        "\"ids_max\":%u,"
        "\"overflow\":%u,"
        "\"bytes_per_sec\":%lu,"
        "\"projected_seconds\":%lu,"
        "\"projected_seconds_nominal\":%lu,"
        "\"tier_b_floor_bytes\":%llu,"
        "\"fs_total_bytes\":%llu,"
        "\"basis\":\"runtime id count; block = u32 ms + u16 count + N*13; "
        "tier B floor = %u%% of the partition\"},",
      (unsigned)ids, (unsigned)SNIFF_MAX_IDS, (unsigned)snifferOverflow(),
      (unsigned long)rate,
      (unsigned long)(rate ? (uint32_t)(tierBFloor / rate) : 0),
      (unsigned long)secs,
      (unsigned long long)tierBFloor,
      (unsigned long long)fsTotal,
      (unsigned)(100 - FS_TIER_A_MAX_PCT));

  // Stream counters. The hub counts datagrams it RECEIVED; these are what the
  // logger SENT. Publishing both is what lets a disagreement be attributed to
  // the network instead of being argued about.
  n = jsonAppend(buf, sizeof(buf), n,
      "\"stream\":{\"packets\":%lu,\"records\":%lu,"
      "\"fast_packets\":%lu,\"fast_records\":%lu,"
      "\"snapshot_hz\":%u,\"fast_hz\":%u},",
      (unsigned long)hubstreamPacketsSent(),
      (unsigned long)hubstreamRecordsSent(),
      (unsigned long)hubstreamFastPacketsSent(),
      (unsigned long)hubstreamFastRecordsSent(),
      (unsigned)HUB_SNAPSHOT_HZ, (unsigned)HUB_FAST_HZ);

  // Storage. Protocol 2.3 says the logger warns "well before" it has to delete
  // anything -- it has no MQTT client, so it reports here and the hub's sync
  // loop publishes to hub/health. `deleted_unacked` is the number that
  // matters: anything above zero means retention destroyed data the hub had
  // not collected, which is the one outcome the tiering exists to prevent.
  const FileStoreStats *fs = filestoreStats();
  n = jsonAppend(buf, sizeof(buf), n,
      "\"storage\":{\"mounted\":%s,\"used\":%lu,\"total\":%lu,"
      "\"usage_pct\":%u,\"warn_pct\":%u,\"warn\":%s,"
      "\"files\":%u,\"open\":%u,\"acked_through\":%ld,"
      "\"tier_a_bytes\":%lu,\"tier_b_bytes\":%lu,\"tier_c_bytes\":%lu,"
      "\"deleted_acked\":%lu,\"deleted_unacked\":%lu,"
      "\"write_errors\":%lu,\"rows_dropped\":%lu},",
      fs->mounted ? "true" : "false",
      (unsigned long)fs->usedBytes, (unsigned long)fs->totalBytes,
      (unsigned)filestoreUsagePct(), (unsigned)FS_WARN_USAGE_PCT,
      (filestoreUsagePct() >= FS_WARN_USAGE_PCT) ? "true" : "false",
      (unsigned)fs->files, (unsigned)fs->openFiles, (long)fs->ackedThrough,
      (unsigned long)fs->tierABytes, (unsigned long)fs->tierBBytes,
      (unsigned long)fs->tierCBytes,
      (unsigned long)fs->deletedAcked, (unsigned long)fs->deletedUnacked,
      (unsigned long)fs->writeErrors, (unsigned long)fs->rowsDropped);

  // Link transition counters, for the AP<->STA soak test. A fault that
  // RECOVERED leaves no other trace.
  const HubLinkStats *ls = hublinkStats();
  n = jsonAppend(buf, sizeof(buf), n,
      "\"link_stats\":{\"sta_joins\":%lu,\"sta_drops\":%lu,"
      "\"event_drops\":%lu,\"poll_drops\":%lu,\"join_failures\":%lu,"
      "\"ap_starts\":%lu,\"last_reason\":%u,"
      "\"last_fallback_ms\":%lu,\"worst_fallback_ms\":%lu},"
      "\"heap\":{\"free\":%lu,\"min_free\":%lu}}",
      (unsigned long)ls->staJoins, (unsigned long)ls->staDrops,
      (unsigned long)ls->eventDrops, (unsigned long)ls->pollDrops,
      (unsigned long)ls->joinFailures, (unsigned long)ls->apStarts,
      (unsigned)ls->lastReason,
      (unsigned long)ls->lastFallbackMs,
      (unsigned long)ls->worstFallbackMs,
      (unsigned long)ESP.getFreeHeap(),
      (unsigned long)ESP.getMinFreeHeap());

  srv.send(200, "application/json", buf);
}

// GET /api/v1/fast -- the current fast list.
static void handleFastGet(WebServer &srv) {
  uint32_t ids[HUB_FAST_MAX_IDS];
  const uint16_t n = hubstreamFastIds(ids, HUB_FAST_MAX_IDS);

  char buf[256];
  int k = snprintf(buf, sizeof(buf), "{\"max\":%u,\"hz\":%u,\"ids\":[",
                   (unsigned)HUB_FAST_MAX_IDS, (unsigned)HUB_FAST_HZ);
  for (uint16_t i = 0; i < n; i++) {
    k += snprintf(buf + k, sizeof(buf) - k, "%s%lu",
                  i ? "," : "", (unsigned long)ids[i]);
  }
  snprintf(buf + k, sizeof(buf) - k, "],\"fast_packets\":%lu}",
           (unsigned long)hubstreamFastPacketsSent());
  srv.send(200, "application/json", buf);
}

// POST /api/v1/fast   {"ids": [145, 348]}   -- decimal or 0x-prefixed.
//
// Protocol 1.5: the hub owns this list. The logger has no idea what an id
// means and must never populate it from a guess, so this endpoint is the only
// way it gets set on a live bus.
static void handleFastPost(WebServer &srv) {
  if (!tokenOk(srv)) { denied(srv); return; }

  const String body = srv.arg("plain");
  const int open = body.indexOf('[');
  const int close = body.indexOf(']', open + 1);
  if (open < 0 || close < 0) {
    srv.send(400, "application/json",
             "{\"error\":\"need {\\\"ids\\\":[...]}\"}");
    return;
  }

  uint32_t ids[HUB_FAST_MAX_IDS];
  uint16_t n = 0;
  bool overflow = false;
  int i = open + 1;
  while (i < close) {
    while (i < close && (body[i] == ' ' || body[i] == ',' || body[i] == '"')) i++;
    if (i >= close) break;
    char *end = nullptr;
    // Base 0: accepts 145 and 0x91 alike. CAN ids get written both ways and
    // silently reading "0x91" as 0 would be a very quiet way to be wrong.
    const unsigned long v = strtoul(body.c_str() + i, &end, 0);
    if (end == body.c_str() + i) break;       // no digits; stop rather than spin
    if (n < HUB_FAST_MAX_IDS) ids[n++] = (uint32_t)v;
    else overflow = true;
    i = (int)(end - body.c_str());
    while (i < close && body[i] == '"') i++;
  }

  const uint16_t applied = hubstreamSetFastIds(n ? ids : nullptr, n);

  char buf[192];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"applied\":%u,\"max\":%u,\"hz\":%u,"
           "\"truncated\":%s}",
           (unsigned)applied, (unsigned)HUB_FAST_MAX_IDS,
           (unsigned)HUB_FAST_HZ, overflow ? "true" : "false");
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

void hubapiRegister(WebServer &srv) {
  // WebServer only retains headers it was told to collect.
  // ONE call for the whole server: collectHeaders REPLACES the retained list,
  // so a second call anywhere else would silently drop whatever this one asked
  // for. "Range" is needed by the filestore's resumable fetch (protocol 2).
  const char *wanted[] = {"X-Hub-Token", "Range"};
  srv.collectHeaders(wanted, 2);

  srv.on("/api/v1/session", HTTP_GET,  [&srv]() { handleSession(srv); });
  srv.on("/api/v1/time",    HTTP_POST, [&srv]() { handleTime(srv); });
  srv.on("/api/v1/fast",    HTTP_GET,  [&srv]() { handleFastGet(srv); });
  srv.on("/api/v1/fast",    HTTP_POST, [&srv]() { handleFastPost(srv); });
}

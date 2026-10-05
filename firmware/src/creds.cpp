#include <Arduino.h>
#include <Preferences.h>
#include <esp_log.h>
#include <stdlib.h>
#include <string.h>

#include "creds.h"
#include "credstore.h"
#include "recorder.h"
#include "config.h"   // pulls in a legacy secrets.h if one exists

// NVS namespace (<= 15 chars). Separate from "cardiag" (mode/radio prefs) so
// `cred clear all` cannot touch anything else.
static const char kNs[] = "cardiaglink";
static const char kImportedKey[] = "imported";
// COMMIT PROTOCOL. `complete` is written 0 before the first field and 1 after
// the last. A power cut anywhere in between leaves 0, and boot treats 0 -- or
// no flag at all -- as NOT provisioned, never as a half-valid set. Each single
// NVS write is itself atomic (ESP-IDF NVS guarantee), so the flag is the only
// multi-key ordering this needs.
static const char kCompleteKey[] = "complete";
static const char kGenKey[] = "gen";

// Entries (32 B each) left free after a commit, for bootguard and the mode
// prefs. The partition is 0x5000: 4 usable pages x 126 entries.
#define CREDS_NVS_RESERVE_ENTRIES 48
#define CREDS_LINE_IDLE_MS 30000

// Active set, loaded ONCE at boot. Nothing reads NVS after setup().
static char g_ssid[CRED_SSID_MAX + 1];
static char g_pass[CRED_PASS_MAX + 1];
static char g_token[CRED_TOKEN_MAX + 1];
static uint8_t g_hubAddr[4];
static bool    g_hubAddrSet = false;
static char    g_hubName[CRED_HUB_NAME_MAX + 1];   // "" = default
static char    g_loggerName[CRED_LOGGER_NAME_MAX + 1];   // "" = default

// Staged changes, applied only by `cred commit`.
enum StageOp : uint8_t { ST_KEEP = 0, ST_SET, ST_CLEAR };
static StageOp g_op[CRED_FIELD_COUNT];
static char    g_stv[CRED_CA][CRED_MQTT_PASS_MAX + 1];   // every field but CA
static size_t  g_stn[CRED_FIELD_COUNT];
// The two CA-sized buffers are heap, allocated only while in use (a staged
// CA; a PEM being pasted or printed). Static, they were 4 kB of DRAM held for
// the whole run to serve a command typed once per board.
#define CA_BUF (CRED_CA_MAX + 1)
static char   *g_stCa = nullptr;

// Console state.
static char     g_line[CRED_LINE_MAX + 1];
static size_t   g_lineLen = 0;
static bool     g_lineOn = false;
static bool     g_lineOverflow = false;
static uint32_t g_lineLastMs = 0;
static bool     g_caOn = false;          // collecting a PEM
static char    *g_ca = nullptr;          // PEM being pasted (CA_BUF)
static size_t   g_caLen = 0;
static bool     g_caOverflow = false;

// credWipe (credstore.h): a wipe the compiler cannot elide before free().
static void wipe(void *p, size_t n) { credWipe(p, n); }

static char *caAlloc() { return (char *)calloc(1, CA_BUF); }
static void caFree(char *&b) {
  if (!b) return;
  wipe(b, CA_BUF);
  free(b);
  b = nullptr;
}

static const char *stagedValue(CredField f) {
  return f == CRED_CA ? g_stCa : g_stv[f];
}

static bool anyStaged() {
  for (uint8_t i = 0; i < CRED_FIELD_COUNT; i++)
    if (g_op[i] != ST_KEEP) return true;
  return false;
}

static void dropStage() {
  wipe(g_stv, sizeof(g_stv));
  caFree(g_stCa);
  memset(g_op, 0, sizeof(g_op));
  memset(g_stn, 0, sizeof(g_stn));
}

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------

// Reads field `f` into buf (NUL-terminated). Returns its length, 0 if absent.
static size_t nvsLoad(Preferences &p, CredField f, char *buf, size_t cap) {
  buf[0] = '\0';
  const char *key = credNvsKey(f);
  if (!p.isKey(key)) return 0;
  const size_t n = p.getBytesLength(key);
  if (n == 0 || n >= cap) return 0;
  if (p.getBytes(key, buf, n) != n) { wipe(buf, cap); return 0; }
  buf[n] = '\0';
  return n;
}

static bool storeEmpty(Preferences &p) {
  for (uint8_t i = 0; i < CRED_FIELD_COUNT; i++)
    if (p.isKey(credNvsKey((CredField)i))) return false;
  return true;
}

static CredCommitState storeState(Preferences &p) {
  return credCommitState(p.isKey(kCompleteKey), p.getUChar(kCompleteKey, 0),
                         !storeEmpty(p));
}

#if defined(WIFI_STA_SSID) || defined(WIFI_STA_PASS) || defined(HUB_API_TOKEN)
#define CREDS_LEGACY 1
#else
#define CREDS_LEGACY 0
#endif

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

static void printField(Preferences &p, CredField f) {
  char small[CRED_MQTT_PASS_MAX + 1];     // every field but the CA
  char *buf = small;
  size_t cap = sizeof(small);
  if (f == CRED_CA) {
    if (!p.isKey(credNvsKey(f))) {
      Serial.printf("[creds]   %-9s (not set)\n", credFieldName(f));
      return;
    }
    buf = caAlloc();
    cap = CA_BUF;
    if (!buf) {
      Serial.printf("[creds]   %-9s set (no memory to fingerprint)\n",
                    credFieldName(f));
      return;
    }
  }
  const size_t n = nvsLoad(p, f, buf, cap);
  char d[CRED_DESC_LEN];
  if (n) credDescribe(f, buf, n, d);
  Serial.printf("[creds]   %-9s %s\n", credFieldName(f), n ? d : "(not set)");
  if (buf == small) wipe(small, sizeof(small));
  else caFree(buf);
}

static void printAll(Preferences &p) {
  for (uint8_t i = 0; i < CRED_FIELD_COUNT; i++) printField(p, (CredField)i);
  // "gen 0" next to INCOMPLETE read as a failed commit on a board that had
  // never had one; a never-provisioned store has no generation to print.
  const CredCommitState st = storeState(p);
  char gen[16] = "";
  if (st != CRED_COMMIT_NONE)
    snprintf(gen, sizeof(gen), " gen %lu", (unsigned long)p.getULong(kGenKey, 0));
  Serial.printf("[creds]   commit: %s%s   legacy import: %s\n",
                credCommitStateName(st), gen,
                p.getUChar(kImportedKey, 0) ? "done" :
                CREDS_LEGACY ? "available (`:cred import`)" : "no secrets.h");
}

// Load one field into the active set, re-validated: a corrupt entry is
// dropped, not joined with.
static void loadActive(Preferences &p, CredField f, char *buf, size_t cap) {
  const size_t n = nvsLoad(p, f, buf, cap);
  if (n && credValidate(f, buf, n) != CRED_OK) {
    Serial.printf("[creds] stored %s fails validation -- ignored\n",
                  credFieldName(f));
    wipe(buf, cap);
  }
}

void credsBegin() {
  // The Wi-Fi driver logs "connected with <SSID>" at INFO, and esp_log is
  // routed to Serial (logq.h). Warnings and errors still come through.
  esp_log_level_set("wifi", ESP_LOG_WARN);

  Preferences p;
  // Read-write so the namespace exists; begin(ro) on a missing namespace fails.
  if (!p.begin(kNs, false)) {
    Serial.println("[creds] NVS open FAILED -- STANDALONE, API token locked");
    return;
  }
  if (storeState(p) != CRED_COMMIT_COMPLETE) {
    // Nothing loaded: an interrupted commit is not half a set.
    if (storeState(p) == CRED_COMMIT_NONE)
      Serial.println("[creds] hub link NOT PROVISIONED -- running STANDALONE: "
                     "own AP only, no hub join. `:cred help` to provision.");
    else
      Serial.println("[creds] hub link INCOMPLETE (interrupted commit) -- "
                     "running STANDALONE, API token locked. Re-provision and "
                     "`:cred commit`.");
  } else {
    loadActive(p, CRED_SSID, g_ssid, sizeof(g_ssid));
    loadActive(p, CRED_PASS, g_pass, sizeof(g_pass));
    loadActive(p, CRED_TOKEN, g_token, sizeof(g_token));
    char a[CRED_HUB_ADDR_MAX + 1];
    loadActive(p, CRED_HUB_ADDR, a, sizeof(a));
    g_hubAddrSet = a[0] && credParseIPv4(a, strlen(a), g_hubAddr);
    loadActive(p, CRED_HUB_NAME, g_hubName, sizeof(g_hubName));
    loadActive(p, CRED_LOGGER_NAME, g_loggerName, sizeof(g_loggerName));
    if (credsHaveHubWifi()) {
      Serial.println("[creds] hub link PROVISIONED (NVS):");
    } else {
      wipe(g_ssid, sizeof(g_ssid));
      wipe(g_pass, sizeof(g_pass));
      Serial.println("[creds] hub link NOT PROVISIONED -- running STANDALONE: "
                     "own AP only, no hub join. `:cred help` to provision.");
    }
  }
  printAll(p);
  if (!g_token[0])
    Serial.println("[creds] no API token -- POST /api/v1/time and "
                   "/api/v1/files/ack answer 401 until one is set");
  p.end();
}

bool        credsHaveHubWifi() { return g_ssid[0] && g_pass[0]; }
const char *credsHubSsid()     { return g_ssid; }
const char *credsHubPass()     { return g_pass; }

const char *credsLoggerName() {
  return g_loggerName[0] ? g_loggerName : CRED_LOGGER_NAME_DEFAULT;
}

const char *credsHubName() {
  return g_hubName[0] ? g_hubName : CRED_HUB_NAME_DEFAULT;
}

bool credsHubAddr(uint8_t out[4]) {
  if (!g_hubAddrSet) return false;
  memcpy(out, g_hubAddr, 4);
  return true;
}

bool credsTokenOk(const char *got, size_t n) {
  return credTokenEqual(g_token, strlen(g_token), got, n);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static void printHelp() {
  Serial.println("cred commands (start the line with ':', end with Enter; "
                 "nothing is echoed):");
  Serial.println("  :cred show                         stored + staged, no values");
  Serial.println("  :cred set ssid|pass|mqtt_user|mqtt_pass|token|hub_addr|hub_name|logger_name <value>");
  Serial.println("  :cred ca                           then paste the PEM");
  Serial.println("  :cred clear <field>|all");
  Serial.println("  :cred commit                       validate all, then save");
  Serial.println("  :cred abort                        drop staged changes");
  Serial.println("  :cred import                       one-time, from secrets.h");
  Serial.println("values are literal to the end of the line (spaces and quotes "
                 "included). set/clear only STAGE; commit writes NVS; a reset "
                 "applies it.");
}

static void stage(CredField f, const char *v, size_t n) {
  const CredErr e = credValidate(f, v, n);
  if (e != CRED_OK) {
    Serial.printf("[creds] %s REJECTED: %s (not staged)\n", credFieldName(f),
                  credErrName(e));
    return;
  }
  if (f == CRED_CA && !g_stCa && !(g_stCa = caAlloc())) {
    Serial.printf("[creds] ca REJECTED: %s (not staged)\n",
                  credErrName(CRED_E_NOMEM));
    return;
  }
  char *dst = (char *)stagedValue(f);
  const size_t cap = (f == CRED_CA) ? CA_BUF : sizeof(g_stv[0]);
  wipe(dst, cap);
  memcpy(dst, v, n);                     // n <= credFieldMax(f) < cap
  g_stn[f] = n;
  g_op[f] = ST_SET;
  char d[CRED_DESC_LEN];
  credDescribe(f, v, n, d);
  Serial.printf("[creds] %s staged (%s) -- `:cred commit` to save\n",
                credFieldName(f), d);
}

static void stageClear(bool all, CredField f) {
  for (uint8_t i = 0; i < CRED_FIELD_COUNT; i++) {
    if (!all && i != f) continue;
    if (i == CRED_CA) caFree(g_stCa);
    else wipe(g_stv[i], sizeof(g_stv[0]));
    g_stn[i] = 0;
    g_op[i] = ST_CLEAR;
  }
  Serial.printf("[creds] clear %s staged -- `:cred commit` to save\n",
                all ? "all" : credFieldName(f));
}

static void printStaged() {
  if (!anyStaged()) { Serial.println("[creds] staged: nothing"); return; }
  Serial.println("[creds] staged (not saved):");
  for (uint8_t i = 0; i < CRED_FIELD_COUNT; i++) {
    if (g_op[i] == ST_KEEP) continue;
    char d[CRED_DESC_LEN] = "clear";
    if (g_op[i] == ST_SET)
      credDescribe((CredField)i, stagedValue((CredField)i), g_stn[i], d);
    Serial.printf("[creds]   %-9s %s\n", credFieldName((CredField)i), d);
  }
}

// Validate the whole resulting set, then write it under the complete flag.
static bool commit() {
  if (!anyStaged()) { Serial.println("[creds] nothing staged"); return false; }
  // Flash writes stall both cores' cache. Not while the change log records a
  // car: stop it first ('l'), commit, restart it.
  if (recorderRunning()) {
    Serial.println("[creds] commit refused: change log running -- 'l' to stop "
                   "it first (NVS writes stall the CPU)");
    return false;
  }
  Preferences p;
  if (!p.begin(kNs, false)) { Serial.println("[creds] NVS open FAILED"); return false; }

  bool present[CRED_FIELD_COUNT];
  size_t need = 4;                       // flag + gen + headroom
  for (uint8_t i = 0; i < CRED_FIELD_COUNT; i++) {
    present[i] = g_op[i] == ST_SET ||
                 (g_op[i] == ST_KEEP && p.isKey(credNvsKey((CredField)i)));
    // A blob costs its data in 32 B entries plus index/header entries.
    if (g_op[i] == ST_SET) need += g_stn[i] / 32 + 3;
  }
  CredField bad = CRED_NONE;
  if (credCheckSet(present, &bad) != CRED_OK) {
    Serial.printf("[creds] commit refused: %s %s -- nothing written\n",
                  credFieldName(bad), "missing (it goes with its pair)");
    p.end();
    return false;
  }
  if (p.freeEntries() < need + CREDS_NVS_RESERVE_ENTRIES) {
    Serial.printf("[creds] commit refused: NVS nearly full (%u free entries) "
                  "-- nothing written\n", (unsigned)p.freeEntries());
    p.end();
    return false;
  }

  bool ok = p.putUChar(kCompleteKey, 0) == 1;
  for (uint8_t i = 0; ok && i < CRED_FIELD_COUNT; i++) {
    const char *key = credNvsKey((CredField)i);
    if (g_op[i] == ST_SET)
      ok = p.putBytes(key, stagedValue((CredField)i), g_stn[i]) == g_stn[i];
    else if (g_op[i] == ST_CLEAR && p.isKey(key))
      ok = p.remove(key);
  }
  if (ok) ok = p.putULong(kGenKey, p.getULong(kGenKey, 0) + 1) == 4;
  if (ok) ok = p.putUChar(kCompleteKey, 1) == 1;
  p.end();
  dropStage();
  if (!ok) {
    Serial.println("[creds] commit FAILED mid-write -- store marked INCOMPLETE; "
                   "boot will run standalone. Re-provision and commit.");
    return false;
  }
  Serial.println("[creds] committed -- reset to apply");
  return true;
}

#if CREDS_LEGACY
static void importOne(CredField f, const char *v) {
  const size_t n = strlen(v);
  const CredErr e = credIsPlaceholder(v, n) ? CRED_E_PLACEHOLDER
                                            : credValidate(f, v, n);
  if (e != CRED_OK) {
    Serial.printf("[creds] import %s skipped: %s\n", credFieldName(f),
                  credErrName(e));
    return;
  }
  stage(f, v, n);
}
#endif

static void doImport() {
#if !CREDS_LEGACY
  Serial.println("[creds] import: this build has no secrets.h values");
#else
  if (anyStaged()) {
    Serial.println("[creds] import refused: changes staged (`:cred abort`)");
    return;
  }
  Preferences p;
  if (!p.begin(kNs, false)) { Serial.println("[creds] NVS open FAILED"); return; }
  const bool done = p.getUChar(kImportedKey, 0) != 0;
  const bool empty = storeEmpty(p);
  p.end();
  // Never overwrites: an EMPTY store only, and only once.
  if (done)   { Serial.println("[creds] import refused: already done once"); return; }
  if (!empty) { Serial.println("[creds] import refused: NVS already provisioned"); return; }
#if defined(WIFI_STA_SSID) && defined(WIFI_STA_PASS)
  // A pair or nothing: an SSID without its passphrase joins nothing.
  const size_t pn = strlen(WIFI_STA_PASS);
  if (!credIsPlaceholder(WIFI_STA_PASS, pn) &&
      credValidate(CRED_PASS, WIFI_STA_PASS, pn) == CRED_OK &&
      credValidate(CRED_SSID, WIFI_STA_SSID, strlen(WIFI_STA_SSID)) == CRED_OK) {
    importOne(CRED_SSID, WIFI_STA_SSID);
    importOne(CRED_PASS, WIFI_STA_PASS);
  } else {
    Serial.println("[creds] import ssid/pass skipped: placeholder or invalid");
  }
#endif
#if defined(HUB_API_TOKEN)
  importOne(CRED_TOKEN, HUB_API_TOKEN);
#endif
  // Nothing real to import does not use up the one time.
  if (!anyStaged()) { Serial.println("[creds] import: nothing to import"); return; }
  if (!commit()) return;
  if (p.begin(kNs, false)) { p.putUChar(kImportedKey, 1); p.end(); }
  Serial.println("[creds] import done (one-time). secrets.h is no longer "
                 "needed; delete its hub values and rebuild.");
#endif
}

static void runLine(const char *line, size_t n) {
  const CredParsed c = credParseLine(line, n);
  switch (c.cmd) {
    case CRED_CMD_NONE:
      Serial.println("[creds] unknown ':' command; `:cred help`");
      break;
    case CRED_CMD_BAD:
      Serial.println("[creds] bad cred command; `:cred help`");
      break;
    case CRED_CMD_HELP:
      printHelp();
      break;
    case CRED_CMD_SHOW: {
      Preferences p;
      if (p.begin(kNs, true)) {
        Serial.println("[creds] stored in NVS:");
        printAll(p);
        p.end();
      } else {
        Serial.println("[creds] nothing stored");
      }
      printStaged();
      break;
    }
    case CRED_CMD_SET:
      stage(c.field, c.value, c.valueLen);
      break;
    case CRED_CMD_CA:
      if (!g_ca && !(g_ca = caAlloc())) {
        Serial.printf("[creds] ca: %s\n", credErrName(CRED_E_NOMEM));
        break;
      }
      g_caOn = true;
      g_caLen = 0;
      g_caOverflow = false;
      Serial.printf("[creds] paste the CA PEM (max %u bytes); ends at "
                    "-----END CERTIFICATE-----\n", (unsigned)CRED_CA_MAX);
      break;
    case CRED_CMD_CLEAR:
      stageClear(c.all, c.field);
      break;
    case CRED_CMD_COMMIT:
      commit();
      break;
    case CRED_CMD_ABORT:
      dropStage();
      Serial.println("[creds] staged changes dropped");
      break;
    case CRED_CMD_IMPORT:
      doImport();
      break;
  }
}

// One PEM line while collecting.
static void caLine(const char *line, size_t n) {
  if (g_caLen + n + 1 > CRED_CA_MAX) g_caOverflow = true;
  if (!g_caOverflow) {
    memcpy(g_ca + g_caLen, line, n);
    g_caLen += n;
    g_ca[g_caLen++] = '\n';
  }
  static const char kEnd[] = "-----END CERTIFICATE-----";
  if (n < sizeof(kEnd) - 1 || memcmp(line, kEnd, sizeof(kEnd) - 1) != 0) return;
  g_caOn = false;
  if (g_caOverflow) {
    Serial.printf("[creds] ca REJECTED: %s (not staged)\n",
                  credErrName(CRED_E_LONG));
  } else {
    stage(CRED_CA, g_ca, g_caLen);
  }
  caFree(g_ca);
  g_caLen = 0;
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------

static void endLine() {
  if (g_lineOverflow) {
    Serial.println("[creds] line too long -- discarded");
    if (g_caOn) {
      g_caOn = false;
      caFree(g_ca);
      g_caLen = 0;
      Serial.println("[creds] ca aborted");
    }
  } else if (g_caOn) {
    if (g_lineLen) caLine(g_line, g_lineLen);
  } else if (g_lineLen) {
    runLine(g_line, g_lineLen);
  }
  wipe(g_line, sizeof(g_line));
  g_lineLen = 0;
  g_lineOverflow = false;
  g_lineOn = g_caOn;                     // stay in line mode for the PEM
}

bool credsConsoleFeed(int ch) {
  if (!g_lineOn) {
    if (ch != ':') return false;
    g_lineOn = true;
    g_lineLen = 0;
    g_lineOverflow = false;
    g_lineLastMs = millis();
    return true;
  }
  g_lineLastMs = millis();
  if (ch == '\r' || ch == '\n') {
    // CRLF: the LF after a CR ends an empty line, which is a no-op.
    endLine();
    return true;
  }
  if (g_lineLen < CRED_LINE_MAX) g_line[g_lineLen++] = (char)ch;
  else g_lineOverflow = true;
  return true;
}

void credsConsoleTick() {
  if (!g_lineOn || millis() - g_lineLastMs < CREDS_LINE_IDLE_MS) return;
  wipe(g_line, sizeof(g_line));
  g_lineLen = 0;
  g_lineOverflow = false;
  if (g_caOn) {
    caFree(g_ca);
    g_caLen = 0;
    g_caOn = false;
    Serial.println("[creds] ca aborted (idle)");
  }
  g_lineOn = false;
  Serial.println("[creds] ':' line timed out -- discarded");
}

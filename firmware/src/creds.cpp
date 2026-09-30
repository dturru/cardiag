#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include "creds.h"
#include "credstore.h"
#include "config.h"   // pulls in a legacy secrets.h if one exists

// NVS namespace (<= 15 chars). Separate from "cardiag" (mode/radio prefs) so
// `cred clear all` cannot touch anything else.
static const char kNs[] = "cardiaglink";
static const char kImportedKey[] = "imported";

#define CREDS_LINE_IDLE_MS 30000

// Active set, loaded once at boot.
static char g_ssid[CRED_SSID_MAX + 1];
static char g_pass[CRED_PASS_MAX + 1];
static char g_token[CRED_TOKEN_MAX + 1];

// Console state.
static char     g_line[CRED_LINE_MAX + 1];
static size_t   g_lineLen = 0;
static bool     g_lineOn = false;
static bool     g_lineOverflow = false;
static uint32_t g_lineLastMs = 0;
static bool     g_caOn = false;          // collecting a PEM
static char     g_ca[CRED_CA_MAX + 1];
static size_t   g_caLen = 0;
static bool     g_caOverflow = false;

// Plain memset can be elided on a buffer that is not read again.
static void wipe(void *p, size_t n) {
  volatile uint8_t *v = (volatile uint8_t *)p;
  while (n--) *v++ = 0;
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

static void fpOf(CredField f, const char *v, size_t n, char fp[CRED_FP_LEN + 1]) {
  if (f == CRED_CA) credCaFingerprint(v, n, fp);
  else credFingerprint((const uint8_t *)v, n, fp);
}

#if defined(WIFI_STA_SSID) || defined(WIFI_STA_PASS) || defined(HUB_API_TOKEN)
#define CREDS_LEGACY 1
#else
#define CREDS_LEGACY 0
#endif

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

static void printField(Preferences &p, CredField f, const char *pad) {
  // CRED_CA_MAX is the largest field; g_ca is idle at boot and during `show`.
  const size_t n = nvsLoad(p, f, g_ca, sizeof(g_ca));
  if (n) {
    char fp[CRED_FP_LEN + 1];
    fpOf(f, g_ca, n, fp);
    Serial.printf("[creds]   %-9s%s %s\n", credFieldName(f), pad, fp);
  } else {
    Serial.printf("[creds]   %-9s%s (not set)\n", credFieldName(f), pad);
  }
  wipe(g_ca, sizeof(g_ca));
}

static void printAll(Preferences &p) {
  for (uint8_t i = 0; i < CRED_FIELD_COUNT; i++) printField(p, (CredField)i, "");
  Serial.printf("[creds]   legacy import: %s\n",
                p.getUChar(kImportedKey, 0) ? "done" :
                CREDS_LEGACY ? "available (`:cred import`)" : "no secrets.h");
}

void credsBegin() {
  Preferences p;
  // Read-write so the namespace exists; begin(ro) on a missing namespace fails.
  if (!p.begin(kNs, false)) {
    Serial.println("[creds] NVS open FAILED -- STANDALONE, API token locked");
    return;
  }
  nvsLoad(p, CRED_SSID, g_ssid, sizeof(g_ssid));
  nvsLoad(p, CRED_PASS, g_pass, sizeof(g_pass));
  nvsLoad(p, CRED_TOKEN, g_token, sizeof(g_token));

  if (credsHaveHubWifi()) {
    Serial.println("[creds] hub link PROVISIONED (NVS):");
  } else {
    Serial.println("[creds] hub link NOT PROVISIONED -- running STANDALONE: "
                   "own AP only, no hub join. `:cred help` to provision.");
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

bool credsTokenOk(const char *got, size_t n) {
  return credTokenEqual(g_token, strlen(g_token), got, n);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static void printHelp() {
  Serial.println("cred commands (start the line with ':', end with Enter; "
                 "nothing is echoed):");
  Serial.println("  :cred show                         fingerprints only");
  Serial.println("  :cred set ssid|pass|mqtt_user|mqtt_pass|token <value>");
  Serial.println("  :cred ca                           then paste the PEM");
  Serial.println("  :cred clear <field>|all");
  Serial.println("  :cred import                       one-time, from secrets.h");
  Serial.println("changes are saved to NVS and apply after a reset.");
}

static void saved(CredField f, const char *v, size_t n) {
  char fp[CRED_FP_LEN + 1];
  fpOf(f, v, n, fp);
  Serial.printf("[creds] %s saved %s -- reset to apply\n", credFieldName(f), fp);
}

static void doSet(CredField f, const char *v, size_t n) {
  const CredErr e = credValidate(f, v, n);
  if (e != CRED_OK) {
    Serial.printf("[creds] %s REJECTED: %s (not saved)\n", credFieldName(f),
                  credErrName(e));
    return;
  }
  Preferences p;
  if (!p.begin(kNs, false) ||
      p.putBytes(credNvsKey(f), v, n) != n) {
    Serial.printf("[creds] %s NVS write FAILED\n", credFieldName(f));
    p.end();
    return;
  }
  p.end();
  saved(f, v, n);
}

static void doClear(bool all, CredField f) {
  Preferences p;
  if (!p.begin(kNs, false)) { Serial.println("[creds] NVS open FAILED"); return; }
  if (all) {
    // Field keys only: the `imported` flag stays, so a stale secrets.h
    // cannot be re-imported behind the operator's back.
    for (uint8_t i = 0; i < CRED_FIELD_COUNT; i++)
      p.remove(credNvsKey((CredField)i));
    Serial.println("[creds] all cleared -- reset to apply");
  } else {
    p.remove(credNvsKey(f));
    Serial.printf("[creds] %s cleared -- reset to apply\n", credFieldName(f));
  }
  p.end();
}

#if CREDS_LEGACY
static bool storeEmpty(Preferences &p) {
  for (uint8_t i = 0; i < CRED_FIELD_COUNT; i++)
    if (p.isKey(credNvsKey((CredField)i))) return false;
  return true;
}

static void importOne(Preferences &p, CredField f, const char *v) {
  const size_t n = strlen(v);
  const CredErr e = credIsPlaceholder(v, n) ? CRED_E_PLACEHOLDER
                                            : credValidate(f, v, n);
  if (e != CRED_OK) {
    Serial.printf("[creds] import %s skipped: %s\n", credFieldName(f),
                  credErrName(e));
    return;
  }
  p.putBytes(credNvsKey(f), v, n);
  saved(f, v, n);
}
#endif

static void doImport() {
#if !CREDS_LEGACY
  Serial.println("[creds] import: this build has no secrets.h values");
#else
  Preferences p;
  if (!p.begin(kNs, false)) { Serial.println("[creds] NVS open FAILED"); return; }
  if (p.getUChar(kImportedKey, 0)) {
    Serial.println("[creds] import refused: already done once");
  } else if (!storeEmpty(p)) {
    Serial.println("[creds] import refused: NVS already provisioned");
  } else {
#if defined(WIFI_STA_SSID) && defined(WIFI_STA_PASS)
    // A pair or nothing: an SSID without its passphrase joins nothing.
    const size_t pn = strlen(WIFI_STA_PASS);
    if (!credIsPlaceholder(WIFI_STA_PASS, pn) &&
        credValidate(CRED_PASS, WIFI_STA_PASS, pn) == CRED_OK &&
        credValidate(CRED_SSID, WIFI_STA_SSID, strlen(WIFI_STA_SSID)) == CRED_OK) {
      importOne(p, CRED_SSID, WIFI_STA_SSID);
      importOne(p, CRED_PASS, WIFI_STA_PASS);
    } else {
      Serial.println("[creds] import ssid/pass skipped: placeholder or invalid");
    }
#endif
#if defined(HUB_API_TOKEN)
    importOne(p, CRED_TOKEN, HUB_API_TOKEN);
#endif
    p.putUChar(kImportedKey, 1);
    Serial.println("[creds] import done (one-time). secrets.h is no longer "
                   "needed; delete it and rebuild.");
  }
  p.end();
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
      if (!p.begin(kNs, true)) { Serial.println("[creds] nothing stored"); break; }
      Serial.println("[creds] stored in NVS:");
      printAll(p);
      p.end();
      break;
    }
    case CRED_CMD_SET:
      doSet(c.field, c.value, c.valueLen);
      break;
    case CRED_CMD_CA:
      g_caOn = true;
      g_caLen = 0;
      g_caOverflow = false;
      Serial.println("[creds] paste the CA PEM; ends at -----END CERTIFICATE-----");
      break;
    case CRED_CMD_CLEAR:
      doClear(c.all, c.field);
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
    Serial.printf("[creds] ca REJECTED: %s (not saved)\n",
                  credErrName(CRED_E_LONG));
  } else {
    doSet(CRED_CA, g_ca, g_caLen);
  }
  wipe(g_ca, sizeof(g_ca));
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
      wipe(g_ca, sizeof(g_ca));
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
    wipe(g_ca, sizeof(g_ca));
    g_caLen = 0;
    g_caOn = false;
    Serial.println("[creds] ca aborted (idle)");
  }
  g_lineOn = false;
  Serial.println("[creds] ':' line timed out -- discarded");
}

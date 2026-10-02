#include "credstore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Field table
// ---------------------------------------------------------------------------

struct FieldDef {
  const char *name;
  const char *key;
  size_t      max;
};

static const FieldDef kFields[CRED_FIELD_COUNT] = {
  {"ssid",      "sta_ssid",  CRED_SSID_MAX},
  {"pass",      "sta_pass",  CRED_PASS_MAX},
  {"mqtt_user", "mqtt_user", CRED_MQTT_USER_MAX},
  {"mqtt_pass", "mqtt_pass", CRED_MQTT_PASS_MAX},
  {"token",     "api_token", CRED_TOKEN_MAX},
  {"hub_addr",  "hub_addr",  CRED_HUB_ADDR_MAX},
  {"ca",        "ca_pem",    CRED_CA_MAX},
};

const char *credFieldName(CredField f) {
  return f < CRED_FIELD_COUNT ? kFields[f].name : "?";
}
const char *credNvsKey(CredField f) {
  return f < CRED_FIELD_COUNT ? kFields[f].key : nullptr;
}
size_t credFieldMax(CredField f) {
  return f < CRED_FIELD_COUNT ? kFields[f].max : 0;
}

CredField credFieldFromName(const char *s, size_t n) {
  for (uint8_t i = 0; i < CRED_FIELD_COUNT; i++) {
    if (strlen(kFields[i].name) == n && memcmp(kFields[i].name, s, n) == 0)
      return (CredField)i;
  }
  return CRED_NONE;
}

const char *credErrName(CredErr e) {
  switch (e) {
    case CRED_OK:            return "ok";
    case CRED_E_EMPTY:       return "empty";
    case CRED_E_SHORT:       return "too short";
    case CRED_E_LONG:        return "too long";
    case CRED_E_CHARSET:     return "bad character";
    case CRED_E_PLACEHOLDER: return "placeholder value";
    case CRED_E_PEM:         return "not a single PEM certificate";
    case CRED_E_PAIR:        return "set without its pair";
    case CRED_E_NOMEM:       return "out of memory";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

static const char *const kPlaceholders[] = {
  "changeme123", "cardiag-default", "carhub-default", "change-me",
};

bool credIsPlaceholder(const char *v, size_t n) {
  for (const char *p : kPlaceholders)
    if (strlen(p) == n && memcmp(p, v, n) == 0) return true;
  return false;
}

static bool allPrintable(const char *v, size_t n, bool spaceOk) {
  for (size_t i = 0; i < n; i++) {
    const uint8_t c = (uint8_t)v[i];
    if (c < 0x20 || c == 0x7F) return false;
    if (c == ' ' && !spaceOk) return false;
  }
  return true;
}

static bool isHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

CredErr credValidate(CredField f, const char *v, size_t n) {
  if (f >= CRED_FIELD_COUNT) return CRED_E_CHARSET;
  if (n == 0) return CRED_E_EMPTY;
  if (n > kFields[f].max) return CRED_E_LONG;
  switch (f) {
    case CRED_SSID:
      // 802.11 allows any 0-32 bytes; a serial console does not.
      return allPrintable(v, n, true) ? CRED_OK : CRED_E_CHARSET;
    case CRED_PASS:
      if (n < CRED_PASS_MIN) return CRED_E_SHORT;
      if (n == 64) {                       // raw PSK: exactly 64 hex
        for (size_t i = 0; i < n; i++)
          if (!isHex(v[i])) return CRED_E_CHARSET;
        return CRED_OK;
      }
      for (size_t i = 0; i < n; i++)       // passphrase: printable ASCII
        if ((uint8_t)v[i] < 0x20 || (uint8_t)v[i] > 0x7E) return CRED_E_CHARSET;
      return credIsPlaceholder(v, n) ? CRED_E_PLACEHOLDER : CRED_OK;
    case CRED_MQTT_USER:
      return allPrintable(v, n, false) ? CRED_OK : CRED_E_CHARSET;
    case CRED_MQTT_PASS:
      if (!allPrintable(v, n, true)) return CRED_E_CHARSET;
      return credIsPlaceholder(v, n) ? CRED_E_PLACEHOLDER : CRED_OK;
    case CRED_TOKEN:
      if (n < CRED_TOKEN_MIN) return CRED_E_SHORT;
      // It travels as an HTTP header value: no spaces, no controls.
      if (!allPrintable(v, n, false)) return CRED_E_CHARSET;
      return credIsPlaceholder(v, n) ? CRED_E_PLACEHOLDER : CRED_OK;
    case CRED_HUB_ADDR: {
      uint8_t ip[4];
      return credParseIPv4(v, n, ip) ? CRED_OK : CRED_E_CHARSET;
    }
    case CRED_CA: {
      // DER scratch on the heap, per call: a CA is only decoded while
      // provisioning or printing the banner, so a resident 2 kB bought nothing.
      uint8_t *der = (uint8_t *)malloc(CRED_CA_MAX);
      if (!der) return CRED_E_NOMEM;
      size_t dl = 0;
      const bool ok = credPemToDer(v, n, der, CRED_CA_MAX, &dl);
      credWipe(der, CRED_CA_MAX);
      free(der);
      return ok ? CRED_OK : CRED_E_PEM;
    }
    default:
      return CRED_E_CHARSET;
  }
}

bool credIsSecret(CredField f) {
  return f == CRED_PASS || f == CRED_MQTT_PASS || f == CRED_TOKEN;
}

CredErr credCheckSet(const bool present[CRED_FIELD_COUNT], CredField *bad) {
  static const CredField pairs[][2] = {{CRED_SSID, CRED_PASS},
                                       {CRED_MQTT_USER, CRED_MQTT_PASS}};
  for (const auto &pr : pairs) {
    if (present[pr[0]] == present[pr[1]]) continue;
    if (bad) *bad = present[pr[0]] ? pr[1] : pr[0];
    return CRED_E_PAIR;
  }
  return CRED_OK;
}

CredCommitState credCommitState(bool flagPresent, uint8_t flag, bool anyField) {
  if (flagPresent && flag == 1) return CRED_COMMIT_COMPLETE;
  if (!flagPresent && !anyField) return CRED_COMMIT_NONE;
  return CRED_COMMIT_INCOMPLETE;
}

const char *credCommitStateName(CredCommitState s) {
  switch (s) {
    case CRED_COMMIT_NONE:       return "not provisioned";
    case CRED_COMMIT_INCOMPLETE: return "INCOMPLETE";
    case CRED_COMMIT_COMPLETE:   return "complete";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// Console line parser
// ---------------------------------------------------------------------------

// Next space-delimited word from [*p, end). Leaves *p at the byte after the
// single separating space (or at end).
static bool nextWord(const char **p, const char *end, const char **w,
                     size_t *wn) {
  while (*p < end && **p == ' ') (*p)++;
  if (*p >= end) return false;
  *w = *p;
  while (*p < end && **p != ' ') (*p)++;
  *wn = (size_t)(*p - *w);
  if (*p < end) (*p)++;
  return true;
}

static bool wordIs(const char *w, size_t n, const char *lit) {
  return strlen(lit) == n && memcmp(w, lit, n) == 0;
}

CredParsed credParseLine(const char *line, size_t n) {
  CredParsed r = {CRED_CMD_NONE, CRED_NONE, false, nullptr, 0};
  while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n')) n--;
  const char *p = line, *end = line + n;
  const char *w;
  size_t wn;

  if (!nextWord(&p, end, &w, &wn) || !wordIs(w, wn, "cred")) return r;
  r.cmd = CRED_CMD_BAD;
  if (!nextWord(&p, end, &w, &wn)) { r.cmd = CRED_CMD_HELP; return r; }

  const char *w2;
  size_t w2n;
  const bool hasArg = nextWord(&p, end, &w2, &w2n);

  if (wordIs(w, wn, "help") && !hasArg)   { r.cmd = CRED_CMD_HELP;   return r; }
  if (wordIs(w, wn, "show") && !hasArg)   { r.cmd = CRED_CMD_SHOW;   return r; }
  if (wordIs(w, wn, "ca") && !hasArg)     { r.cmd = CRED_CMD_CA;     return r; }
  if (wordIs(w, wn, "import") && !hasArg) { r.cmd = CRED_CMD_IMPORT; return r; }
  if (wordIs(w, wn, "commit") && !hasArg) { r.cmd = CRED_CMD_COMMIT; return r; }
  if (wordIs(w, wn, "abort") && !hasArg)  { r.cmd = CRED_CMD_ABORT;  return r; }

  if (wordIs(w, wn, "clear") && hasArg) {
    const char *w3;
    size_t w3n;
    if (nextWord(&p, end, &w3, &w3n)) return r;          // trailing junk
    if (wordIs(w2, w2n, "all")) {
      r.cmd = CRED_CMD_CLEAR;
      r.all = true;
      return r;
    }
    const CredField f = credFieldFromName(w2, w2n);
    if (f == CRED_NONE) return r;
    r.cmd = CRED_CMD_CLEAR;
    r.field = f;
    return r;
  }

  if (wordIs(w, wn, "set") && hasArg) {
    const CredField f = credFieldFromName(w2, w2n);
    // The CA is multi-line and goes through `cred ca`.
    if (f == CRED_NONE || f == CRED_CA) return r;
    // Value: everything after the ONE space that ended the field name.
    r.cmd = CRED_CMD_SET;
    r.field = f;
    r.value = p;
    r.valueLen = (size_t)(end - p);
    return r;
  }
  return r;
}

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4). Portable, so the fingerprint is the same function on
// the host test and the board.
// ---------------------------------------------------------------------------

static const uint32_t K256[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
  0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
  0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
  0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void shaBlock(uint32_t h[8], const uint8_t *b) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 |
           (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], bb = h[1], c = h[2], d = h[3];
  uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    const uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) +
                        ((e & f) ^ (~e & g)) + K256[i] + w[i];
    const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) +
                        ((a & bb) ^ (a & c) ^ (bb & c));
    hh = g; g = f; f = e; e = d + t1;
    d = c; c = bb; bb = a; a = t1 + t2;
  }
  h[0] += a; h[1] += bb; h[2] += c; h[3] += d;
  h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void credSha256(const uint8_t *d, size_t n, uint8_t out[32]) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  size_t i = 0;
  for (; i + 64 <= n; i += 64) shaBlock(h, d + i);
  uint8_t tail[128] = {0};
  const size_t rem = n - i;
  if (rem) memcpy(tail, d + i, rem);
  tail[rem] = 0x80;
  const size_t tl = (rem < 56) ? 64 : 128;
  const uint64_t bits = (uint64_t)n * 8;
  for (int k = 0; k < 8; k++) tail[tl - 1 - k] = (uint8_t)(bits >> (8 * k));
  shaBlock(h, tail);
  if (tl == 128) shaBlock(h, tail + 64);
  for (int k = 0; k < 8; k++) {
    out[4 * k]     = (uint8_t)(h[k] >> 24);
    out[4 * k + 1] = (uint8_t)(h[k] >> 16);
    out[4 * k + 2] = (uint8_t)(h[k] >> 8);
    out[4 * k + 3] = (uint8_t)h[k];
  }
}

void credFingerprint(const uint8_t *d, size_t n, char out[CRED_FP_LEN + 1]) {
  static const char hex[] = "0123456789abcdef";
  uint8_t dig[32];
  credSha256(d, n, dig);
  memcpy(out, "sha256:", 7);
  for (int i = 0; i < 4; i++) {
    out[7 + 2 * i]     = hex[dig[i] >> 4];
    out[7 + 2 * i + 1] = hex[dig[i] & 0xF];
  }
  out[CRED_FP_LEN] = '\0';
}

// ---------------------------------------------------------------------------
// PEM
// ---------------------------------------------------------------------------

static const char kBegin[] = "-----BEGIN CERTIFICATE-----";
static const char kEnd[]   = "-----END CERTIFICATE-----";

static const char *findIn(const char *h, size_t hn, const char *needle) {
  const size_t nn = strlen(needle);
  for (size_t i = 0; i + nn <= hn; i++)
    if (memcmp(h + i, needle, nn) == 0) return h + i;
  return nullptr;
}

static int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool credPemToDer(const char *pem, size_t n, uint8_t *der, size_t cap,
                  size_t *derLen) {
  *derLen = 0;
  if (n > CRED_CA_MAX) return false;
  const char *b = findIn(pem, n, kBegin);
  if (!b) return false;
  const char *body = b + strlen(kBegin);
  const char *e = findIn(body, (size_t)(pem + n - body), kEnd);
  if (!e) return false;
  // Exactly one certificate: nothing but whitespace outside the block.
  for (const char *q = pem; q < b; q++)
    if (*q != ' ' && *q != '\r' && *q != '\n' && *q != '\t') return false;
  for (const char *q = e + strlen(kEnd); q < pem + n; q++)
    if (*q != ' ' && *q != '\r' && *q != '\n' && *q != '\t') return false;

  uint32_t acc = 0;
  int bits = 0, pad = 0;
  size_t out = 0, chars = 0;
  for (const char *q = body; q < e; q++) {
    const char c = *q;
    if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
    if (c == '=') { pad++; chars++; continue; }
    if (pad) return false;                 // data after padding
    const int v = b64val(c);
    if (v < 0) return false;
    chars++;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (out >= cap) return false;
      der[out++] = (uint8_t)(acc >> bits);
    }
  }
  if (chars == 0 || chars % 4 != 0 || pad > 2) return false;
  // A DER certificate is a SEQUENCE.
  if (out < 2 || der[0] != 0x30) return false;
  *derLen = out;
  return true;
}

void credCaFingerprint(const char *pem, size_t n, char out[CRED_FP_LEN + 1]) {
  uint8_t *der = (uint8_t *)malloc(CRED_CA_MAX);   // see credValidate
  size_t dl = 0;
  if (!der || !credPemToDer(pem, n, der, CRED_CA_MAX, &dl)) {
    if (der) credWipe(der, CRED_CA_MAX);
    free(der);
    memcpy(out, "sha256:????????", CRED_FP_LEN + 1);
    return;
  }
  credFingerprint(der, dl, out);
  credWipe(der, CRED_CA_MAX);
  free(der);
}

bool credParseIPv4(const char *v, size_t n, uint8_t out[4]) {
  size_t i = 0;
  for (int part = 0; part < 4; part++) {
    if (part && (i >= n || v[i++] != '.')) return false;
    const size_t start = i;
    unsigned val = 0;
    while (i < n && v[i] >= '0' && v[i] <= '9' && i - start < 3)
      val = val * 10 + (unsigned)(v[i++] - '0');
    const size_t len = i - start;
    if (len == 0 || val > 255) return false;
    if (len > 1 && v[start] == '0') return false;      // no leading zeros
    out[part] = (uint8_t)val;
  }
  if (i != n) return false;
  const uint32_t all = (uint32_t)out[0] << 24 | (uint32_t)out[1] << 16 |
                       (uint32_t)out[2] << 8 | out[3];
  return all != 0 && all != 0xFFFFFFFFu;
}

void credDescribe(CredField f, const char *v, size_t n, char out[CRED_DESC_LEN]) {
  if (f == CRED_HUB_ADDR) {                 // an address: shown as itself
    snprintf(out, CRED_DESC_LEN, "%.*s", (int)n, v);
  } else if (credIsSecret(f)) {
    snprintf(out, CRED_DESC_LEN, "set, %u chars", (unsigned)n);
  } else if (f == CRED_CA) {
    credCaFingerprint(v, n, out);
  } else {
    credFingerprint((const uint8_t *)v, n, out);
  }
}

void credWipe(void *p, size_t n) {
  volatile uint8_t *v = (volatile uint8_t *)p;
  while (n--) *v++ = 0;
#if defined(__GNUC__)
  // Belt and braces: the buffer's memory is observed, so no store above is dead.
  __asm__ __volatile__("" : : "r"(p) : "memory");
#endif
}

// ---------------------------------------------------------------------------
// Token compare
// ---------------------------------------------------------------------------

bool credTokenEqual(const char *want, size_t wlen, const char *got,
                    size_t glen) {
  if (wlen == 0) return false;
  // The length difference is folded in as a full-width comparison. The old
  // `(uint8_t)(glen ^ wlen)` truncated it, so a header 256 bytes longer than
  // the token, with the token as its prefix, compared EQUAL.
  uint8_t diff = (glen != wlen) ? 1 : 0;
  for (size_t i = 0; i < wlen; i++) {
    const char c = (i < glen) ? got[i] : 0;
    diff |= (uint8_t)(c ^ want[i]);
  }
  return diff == 0;
}

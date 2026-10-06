#include "pollplan.h"

#include <stdio.h>
#include <string.h>

#include "credstore.h"   // credSha256

// ---------------------------------------------------------------------------
// JSON. Two layers: a complete syntax walk (so "malformed" -> 400 means what
// it says), then extraction of the one shape the contract allows (-> 422).
// ---------------------------------------------------------------------------

struct Cur {
  const char *p, *end;
  int depth;
};

static void ws(Cur &c) {
  while (c.p < c.end && (*c.p == ' ' || *c.p == '\t' || *c.p == '\r' ||
                         *c.p == '\n'))
    c.p++;
}

static bool peek(Cur &c, char ch) {
  ws(c);
  return c.p < c.end && *c.p == ch;
}

static bool eat(Cur &c, char ch) {
  if (!peek(c, ch)) return false;
  c.p++;
  return true;
}

static bool isDigit(char ch) { return ch >= '0' && ch <= '9'; }
static bool isHex(char ch) {
  return isDigit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

// String; copies up to cap-1 bytes into out when out != nullptr. *fits is
// false if it did not fit or contained an escape (no key we accept has one).
static bool jstr(Cur &c, char *out, size_t cap, bool *fits) {
  if (!eat(c, '"')) return false;
  size_t n = 0;
  bool ok = true;
  while (c.p < c.end && *c.p != '"') {
    const unsigned char ch = (unsigned char)*c.p;
    if (ch < 0x20) return false;
    if (ch == '\\') {
      ok = false;
      c.p++;
      if (c.p >= c.end) return false;
      const char e = *c.p;
      if (e == 'u') {
        for (int k = 0; k < 4; k++) {
          c.p++;
          if (c.p >= c.end || !isHex(*c.p)) return false;
        }
      } else if (!strchr("\"\\/bfnrt", e)) {
        return false;
      }
      c.p++;
      continue;
    }
    if (out && n + 1 < cap) out[n++] = (char)ch;
    else ok = false;
    c.p++;
  }
  if (c.p >= c.end) return false;
  c.p++;
  if (out) out[n] = '\0';
  if (fits) *fits = ok;
  return true;
}

// Number per RFC 8259. *isInt and *v are set for an optional '-' followed by
// digits only; v saturates at 2^40 (anything that large is out of range).
static bool jnum(Cur &c, bool *isInt, int64_t *v) {
  ws(c);
  bool neg = false;
  if (c.p < c.end && *c.p == '-') { neg = true; c.p++; }
  if (c.p >= c.end || !isDigit(*c.p)) return false;
  int64_t x = 0;
  if (*c.p == '0') {
    c.p++;
  } else {
    while (c.p < c.end && isDigit(*c.p)) {
      if (x < ((int64_t)1 << 40)) x = x * 10 + (*c.p - '0');
      c.p++;
    }
  }
  bool integer = true;
  if (c.p < c.end && *c.p == '.') {
    integer = false;
    c.p++;
    if (c.p >= c.end || !isDigit(*c.p)) return false;
    while (c.p < c.end && isDigit(*c.p)) c.p++;
  }
  if (c.p < c.end && (*c.p == 'e' || *c.p == 'E')) {
    integer = false;
    c.p++;
    if (c.p < c.end && (*c.p == '+' || *c.p == '-')) c.p++;
    if (c.p >= c.end || !isDigit(*c.p)) return false;
    while (c.p < c.end && isDigit(*c.p)) c.p++;
  }
  if (isInt) *isInt = integer;
  if (v) *v = neg ? -x : x;
  return true;
}

static bool jlit(Cur &c, const char *lit) {
  ws(c);
  const size_t n = strlen(lit);
  if ((size_t)(c.end - c.p) < n || memcmp(c.p, lit, n) != 0) return false;
  c.p += n;
  return true;
}

// Skip any value, checking syntax. Depth-limited.
static bool jskip(Cur &c) {
  ws(c);
  if (c.p >= c.end) return false;
  switch (*c.p) {
    case '"': return jstr(c, nullptr, 0, nullptr);
    case 't': return jlit(c, "true");
    case 'f': return jlit(c, "false");
    case 'n': return jlit(c, "null");
    case '{': {
      if (++c.depth > 16) return false;
      c.p++;
      if (eat(c, '}')) { c.depth--; return true; }
      for (;;) {
        if (!jstr(c, nullptr, 0, nullptr) || !eat(c, ':') || !jskip(c)) return false;
        if (eat(c, ',')) continue;
        if (eat(c, '}')) { c.depth--; return true; }
        return false;
      }
    }
    case '[': {
      if (++c.depth > 16) return false;
      c.p++;
      if (eat(c, ']')) { c.depth--; return true; }
      for (;;) {
        if (!jskip(c)) return false;
        if (eat(c, ',')) continue;
        if (eat(c, ']')) { c.depth--; return true; }
        return false;
      }
    }
    default:
      return jnum(c, nullptr, nullptr);
  }
}

static PollPlanResult res(PollPlanStatus s, const char *e) {
  PollPlanResult r = {s, e};
  return r;
}

// An integer value for a known key; "invalid type" if it is any other value.
static bool jint(Cur &c, int64_t *v) {
  const Cur save = c;
  bool isInt = false;
  Cur t = c;
  if (jnum(t, &isInt, v) && isInt) { c = t; return true; }
  c = save;
  return false;
}

static int cmpEntry(const PollEntry &a, const PollEntry &b) {
  if (a.mode != b.mode) return a.mode < b.mode ? -1 : 1;
  if (a.pid != b.pid) return a.pid < b.pid ? -1 : 1;
  return 0;
}

PollPlanResult pollPlanParse(const char *json, size_t n, PollPlan *out) {
  memset(out, 0, sizeof(*out));
  if (n > POLL_JSON_MAX) return res(POLLPLAN_MALFORMED, "body too long");

  // Layer 1: well-formed JSON at all?
  {
    Cur c = {json, json + n, 0};
    if (!jskip(c)) return res(POLLPLAN_MALFORMED, "malformed JSON");
    ws(c);
    if (c.p != c.end) return res(POLLPLAN_MALFORMED, "malformed JSON");
  }

  // Layer 2: the contract. Syntax is known good from here, so every failure
  // below is a 422.
  Cur c = {json, json + n, 0};
  if (!eat(c, '{')) return res(POLLPLAN_INVALID, "body must be an object");
  bool hasVersion = false, hasEntries = false;
  if (!eat(c, '}')) {
    for (;;) {
      char key[16];
      bool fits = false;
      jstr(c, key, sizeof(key), &fits);
      eat(c, ':');
      if (fits && !strcmp(key, "version")) {
        int64_t v;
        if (!jint(c, &v)) return res(POLLPLAN_INVALID, "invalid type: version");
        if (v != 1) return res(POLLPLAN_INVALID, "unsupported version");
        hasVersion = true;
      } else if (fits && !strcmp(key, "entries")) {
        if (!eat(c, '[')) return res(POLLPLAN_INVALID, "invalid type: entries");
        hasEntries = true;
        if (!eat(c, ']')) {
          for (;;) {
            if (out->n >= POLL_PLAN_MAX) return res(POLLPLAN_INVALID, "too many entries");
            if (!eat(c, '{')) return res(POLLPLAN_INVALID, "invalid type: entry");
            int64_t mode = -1, pid = -1, period = -1;
            bool hm = false, hp = false, hq = false;
            if (!eat(c, '}')) {
              for (;;) {
                char k[16];
                bool f = false;
                jstr(c, k, sizeof(k), &f);
                eat(c, ':');
                int64_t v;
                bool *seen;
                if (f && !strcmp(k, "mode"))           seen = &hm;
                else if (f && !strcmp(k, "pid"))       seen = &hp;
                else if (f && !strcmp(k, "period_ms")) seen = &hq;
                else return res(POLLPLAN_INVALID, "unknown field");
                if (*seen) return res(POLLPLAN_INVALID, "duplicate field");
                if (!jint(c, &v)) return res(POLLPLAN_INVALID, "invalid type");
                *seen = true;
                if (seen == &hm) mode = v;
                else if (seen == &hp) pid = v;
                else period = v;
                if (eat(c, ',')) continue;
                eat(c, '}');
                break;
              }
            }
            if (!hm || !hp || !hq) return res(POLLPLAN_INVALID, "missing field");
            if (mode != 1) return res(POLLPLAN_INVALID, "unsupported mode");
            if (pid < 0 || pid > 255) return res(POLLPLAN_INVALID, "pid out of range");
            if (period < (int64_t)POLL_PERIOD_MIN_MS || period > (int64_t)POLL_PERIOD_MAX_MS)
              return res(POLLPLAN_INVALID, "period_ms out of range");
            PollEntry e = {(uint8_t)mode, (uint8_t)pid, (uint32_t)period};
            // Insert sorted; equal key = duplicate.
            uint8_t at = out->n;
            for (uint8_t i = 0; i < out->n; i++) {
              const int k = cmpEntry(e, out->e[i]);
              if (k == 0) return res(POLLPLAN_INVALID, "duplicate entry");
              if (k < 0) { at = i; break; }
            }
            for (uint8_t i = out->n; i > at; i--) out->e[i] = out->e[i - 1];
            out->e[at] = e;
            out->n++;
            if (eat(c, ',')) continue;
            eat(c, ']');
            break;
          }
        }
      } else {
        return res(POLLPLAN_INVALID, "unknown field");
      }
      if (eat(c, ',')) continue;
      eat(c, '}');
      break;
    }
  }
  if (!hasVersion || !hasEntries) {
    memset(out, 0, sizeof(*out));
    return res(POLLPLAN_INVALID, "missing field");
  }
  return res(POLLPLAN_OK, nullptr);
}

static bool planValid(const PollPlan &p) {
  if (p.n > POLL_PLAN_MAX) return false;
  for (uint8_t i = 0; i < p.n; i++) {
    const PollEntry &e = p.e[i];
    if (e.mode != 1) return false;
    if (e.periodMs < POLL_PERIOD_MIN_MS || e.periodMs > POLL_PERIOD_MAX_MS) return false;
    if (i && cmpEntry(p.e[i - 1], e) >= 0) return false;   // sorted, unique
  }
  return true;
}

size_t pollPlanCanonical(const PollPlan &p, char *buf, size_t cap) {
  size_t n = 0;
  int w = snprintf(buf, cap, "{\"version\":1,\"entries\":[");
  if (w < 0 || (size_t)w >= cap) return 0;
  n = (size_t)w;
  for (uint8_t i = 0; i < p.n; i++) {
    w = snprintf(buf + n, cap - n, "%s{\"mode\":%u,\"pid\":%u,\"period_ms\":%lu}",
                 i ? "," : "", (unsigned)p.e[i].mode, (unsigned)p.e[i].pid,
                 (unsigned long)p.e[i].periodMs);
    if (w < 0 || (size_t)w >= cap - n) return 0;
    n += (size_t)w;
  }
  w = snprintf(buf + n, cap - n, "]}");
  if (w < 0 || (size_t)w >= cap - n) return 0;
  return n + (size_t)w;
}

void pollPlanHash(const PollPlan &p, char out[POLL_HASH_HEX + 1]) {
  static const char hex[] = "0123456789abcdef";
  // Static, not on the stack: this runs inside handleSession on the 8 kB loop
  // stack, and 1.5 kB here plus the session buffer and printf overflowed it
  // (bench 4, stackguard.h). Every caller is on the loop task (session GET,
  // pollplan POST, pollerBegin in setup), so one buffer is enough.
  static char canon[POLLPLAN_CANON_MAX];
  const size_t n = pollPlanCanonical(p, canon, sizeof(canon));
  uint8_t d[32];
  credSha256((const uint8_t *)canon, n, d);
  for (int i = 0; i < POLL_HASH_HEX / 2; i++) {
    out[2 * i] = hex[d[i] >> 4];
    out[2 * i + 1] = hex[d[i] & 0xF];
  }
  out[POLL_HASH_HEX] = '\0';
}

// ---------------------------------------------------------------------------
// Encoding + hash
// ---------------------------------------------------------------------------

size_t pollPlanEncode(const PollPlan &p, uint8_t *buf, size_t cap) {
  const size_t need = 2 + (size_t)p.n * 6;
  if (p.n > POLL_PLAN_MAX || cap < need) return 0;
  buf[0] = 1;
  buf[1] = p.n;
  uint8_t *q = buf + 2;
  for (uint8_t i = 0; i < p.n; i++) {
    *q++ = p.e[i].mode;
    *q++ = p.e[i].pid;
    for (int b = 0; b < 4; b++) *q++ = (uint8_t)(p.e[i].periodMs >> (8 * b));
  }
  return need;
}

bool pollPlanDecode(const uint8_t *buf, size_t n, PollPlan *out) {
  memset(out, 0, sizeof(*out));
  if (n < 2 || buf[0] != 1 || buf[1] > POLL_PLAN_MAX) return false;
  if (n != 2 + (size_t)buf[1] * 6) return false;
  out->n = buf[1];
  const uint8_t *q = buf + 2;
  for (uint8_t i = 0; i < out->n; i++) {
    out->e[i].mode = q[0];
    out->e[i].pid = q[1];
    out->e[i].periodMs = (uint32_t)q[2] | (uint32_t)q[3] << 8 |
                         (uint32_t)q[4] << 16 | (uint32_t)q[5] << 24;
    q += 6;
  }
  // NVS is not trusted to still hold a valid plan: re-validate.
  return planValid(*out);
}


// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------

static inline bool reached(uint32_t now, uint32_t t) {
  return (int32_t)(now - t) >= 0;
}

void pollSchedInit(PollSched &s, const PollPlan &p, uint32_t now) {
  memset(&s, 0, sizeof(s));
  s.plan = p;
  for (uint8_t i = 0; i < p.n; i++) s.due[i] = now;
  s.inflight = -1;
}

int pollSchedPick(const PollSched &s, uint32_t now, uint32_t gapMs) {
  if (s.inflight >= 0 || s.plan.n == 0) return -1;
  if (s.anyTx && (int32_t)(now - s.lastTxMs) < (int32_t)gapMs) return -1;
  int best = -1;
  int32_t bestLate = -1;
  for (uint8_t i = 0; i < s.plan.n; i++) {
    if (!reached(now, s.due[i])) continue;
    const int32_t late = (int32_t)(now - s.due[i]);
    if (late > bestLate) { bestLate = late; best = i; }   // most overdue; ties -> lowest index
  }
  return best;
}

void pollSchedSent(PollSched &s, int i, uint32_t now) {
  if (i < 0 || i >= s.plan.n) return;
  s.inflight = (int8_t)i;
  s.sentMs = now;
  s.lastTxMs = now;
  s.anyTx = true;
  s.st[i].requests++;
}

void pollSchedTxFailed(PollSched &s, int i, uint32_t now) {
  if (i < 0 || i >= s.plan.n) return;
  s.st[i].txFailed++;
  s.due[i] = now + s.plan.e[i].periodMs;
  if (s.inflight == i) s.inflight = -1;
}

PollRx pollSchedOnFrame(PollSched &s, const uint8_t *data, uint8_t dlc,
                        uint32_t now, uint8_t *payload, uint8_t *len) {
  if (s.inflight < 0 || dlc < 3) return POLL_RX_IGNORED;
  const int i = s.inflight;
  const PollEntry &e = s.plan.e[i];
  const uint8_t wantMode = (uint8_t)(e.mode + 0x40);
  const uint8_t pci = data[0];

  if ((pci & 0xF0) == 0x10) {
    // First Frame: bytes 2..3 are mode echo + pid echo.
    if (dlc < 4 || data[2] != wantMode || data[3] != e.pid) return POLL_RX_IGNORED;
    s.st[i].multiframe++;
    s.st[i].misses = 0;
    s.st[i].lastLatencyMs = now - s.sentMs;
    s.due[i] = s.sentMs + e.periodMs;
    s.inflight = -1;
    s.anyOk = true;
    return POLL_RX_MULTIFRAME;
  }
  if ((pci & 0xF0) != 0x00) return POLL_RX_IGNORED;     // CF / FC: not ours
  if (pci < 2 || pci > 7 || pci + 1 > dlc) return POLL_RX_IGNORED;
  if (data[1] != wantMode || data[2] != e.pid) return POLL_RX_IGNORED;

  const uint8_t n = (uint8_t)(pci - 2);
  if (payload) memcpy(payload, &data[3], n);
  if (len) *len = n;
  s.st[i].replies++;
  s.st[i].misses = 0;
  s.st[i].lastLatencyMs = now - s.sentMs;
  // Cadence from the send time, not from "now": a slow reply does not push
  // every later sample back by its latency.
  s.due[i] = s.sentMs + e.periodMs;
  s.inflight = -1;
  s.anyOk = true;
  return POLL_RX_OK;
}

uint32_t pollBackoffMs(uint32_t periodMs, uint8_t misses, uint32_t maxMs) {
  const uint32_t cap = maxMs > periodMs ? maxMs : periodMs;
  uint64_t b = periodMs;
  for (uint8_t k = 0; k < misses && b < cap; k++) b <<= 1;
  return b > cap ? cap : (uint32_t)b;
}

bool pollSchedCheckTimeout(PollSched &s, uint32_t now, uint32_t timeoutMs,
                           uint32_t backoffMaxMs) {
  if (s.inflight < 0) return false;
  if ((int32_t)(now - s.sentMs) < (int32_t)timeoutMs) return false;
  const int i = s.inflight;
  s.st[i].timeouts++;
  if (s.st[i].misses < 16) s.st[i].misses++;
  s.due[i] = now + pollBackoffMs(s.plan.e[i].periodMs, s.st[i].misses,
                                 backoffMaxMs);
  s.inflight = -1;
  return true;
}

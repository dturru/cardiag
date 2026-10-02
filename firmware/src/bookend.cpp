#include "bookend.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Record
// ---------------------------------------------------------------------------

void bookendInit(BookendData &d, uint8_t kind) {
  memset(&d, 0, sizeof(d));
  d.kind = kind;
  memset(d.pid01, 0xFF, sizeof(d.pid01));   // "no response" until one arrives
}

static uint8_t *put16(uint8_t *q, uint16_t v) {
  q[0] = (uint8_t)v;
  q[1] = (uint8_t)(v >> 8);
  return q + 2;
}

static uint8_t *put32(uint8_t *q, uint32_t v) {
  for (int i = 0; i < 4; i++) q[i] = (uint8_t)(v >> (8 * i));
  return q + 4;
}

size_t bookendEncode(const BookendData &d, uint32_t deviceId, uint32_t bootId,
                     uint32_t ms, uint8_t *buf, size_t cap) {
  const uint8_t ns = d.nStored > BOOKEND_MAX_STORED ? BOOKEND_MAX_STORED : d.nStored;
  const uint8_t np = d.nPending > BOOKEND_MAX_PENDING ? BOOKEND_MAX_PENDING : d.nPending;
  const uint8_t nm = d.nM06 > BOOKEND_MAX_M06 ? BOOKEND_MAX_M06 : d.nM06;
  const size_t payload = 4 + 4 + 1 + 2u * ns + 1 + 2u * np + 1 + 9u * nm;
  const size_t total = BOOKEND_REC_HDR + payload;
  if (cap < total) return 0;

  uint8_t *q = buf;
  *q++ = HUB_REC_BOOKEND;
  *q++ = BOOKEND_VERSION;
  q = put16(q, (uint16_t)payload);
  q = put32(q, deviceId);
  q = put32(q, bootId);
  q = put32(q, ms);

  *q++ = d.kind;
  *q++ = d.flags;
  q = put16(q, 0);
  memcpy(q, d.pid01, 4);
  q += 4;
  *q++ = ns;
  for (uint8_t i = 0; i < ns; i++) q = put16(q, d.stored[i]);
  *q++ = np;
  for (uint8_t i = 0; i < np; i++) q = put16(q, d.pending[i]);
  *q++ = nm;
  for (uint8_t i = 0; i < nm; i++) {
    *q++ = d.m06[i].mid;
    *q++ = d.m06[i].tid;
    *q++ = d.m06[i].uasid;
    q = put16(q, d.m06[i].value);
    q = put16(q, d.m06[i].min);
    q = put16(q, d.m06[i].max);
  }
  return (size_t)(q - buf);
}

void bookendFileHeader(uint8_t out[BOOKEND_FILE_HDR], uint8_t mode,
                       uint32_t deviceId, uint32_t bootId) {
  memcpy(out, "CDGB", 4);
  out[4] = BOOKEND_VERSION;
  out[5] = HUB_REC_BOOKEND;
  out[6] = mode;
  out[7] = 0;
  put32(out + 8, deviceId);
  put32(out + 12, bootId);
}

// ---------------------------------------------------------------------------
// Parsers
// ---------------------------------------------------------------------------

bool bookendParsePid01(BookendData &d, const uint8_t *p, size_t n) {
  if (n < 6 || p[0] != 0x41 || p[1] != 0x01) return false;
  memcpy(d.pid01, p + 2, 4);
  return true;
}

bool bookendParseDtcs(BookendData &d, bool pending, const uint8_t *p, size_t n) {
  if (n < 2 || p[0] != (pending ? 0x47 : 0x43)) return false;
  // CAN (ISO 15765-4): byte 1 is the DTC count. Trust the bytes present over
  // the count if they disagree.
  size_t count = p[1];
  if (count > (n - 2) / 2) count = (n - 2) / 2;
  uint16_t *arr = pending ? d.pending : d.stored;
  uint8_t &have = pending ? d.nPending : d.nStored;
  const uint8_t cap = pending ? BOOKEND_MAX_PENDING : BOOKEND_MAX_STORED;
  for (size_t i = 0; i < count; i++) {
    const uint16_t dtc = (uint16_t)(p[2 + 2 * i] << 8 | p[3 + 2 * i]);
    if (dtc == 0) continue;                 // padding, not a code
    if (have >= cap) { d.flags |= BOOKEND_F_TRUNCATED; break; }
    arr[have++] = dtc;
  }
  return true;
}

bool bookendParseM06Support(uint8_t base, const uint8_t *p, size_t n,
                            uint8_t supported[32]) {
  if (n < 6 || p[0] != 0x46 || p[1] != base) return false;
  for (int bit = 0; bit < 32; bit++) {
    if (p[2 + bit / 8] & (0x80 >> (bit % 8))) {
      const unsigned mid = base + 1u + (unsigned)bit;
      if (mid <= 0xFF) supported[mid / 8] |= (uint8_t)(1u << (mid % 8));
    }
  }
  return true;
}

bool bookendParseM06Results(BookendData &d, const uint8_t *p, size_t n) {
  if (n < 1 + 9 || p[0] != 0x46) return false;
  for (size_t o = 1; o + 9 <= n; o += 9) {
    if (d.nM06 >= BOOKEND_MAX_M06) { d.flags |= BOOKEND_F_TRUNCATED; break; }
    BookendM06 &m = d.m06[d.nM06++];
    m.mid   = p[o];
    m.tid   = p[o + 1];
    m.uasid = p[o + 2];
    m.value = (uint16_t)(p[o + 3] << 8 | p[o + 4]);
    m.min   = (uint16_t)(p[o + 5] << 8 | p[o + 6]);
    m.max   = (uint16_t)(p[o + 7] << 8 | p[o + 8]);
  }
  return true;
}

// ---------------------------------------------------------------------------
// ISO-TP receive
// ---------------------------------------------------------------------------

IsoTpEv isoTpFeed(IsoTpRx &r, const uint8_t *d, uint8_t dlc) {
  if (dlc < 1) return ISOTP_NONE;
  switch (d[0] >> 4) {
    case 0x0: {                                   // Single Frame
      const uint8_t len = d[0] & 0x0F;
      r.active = false;
      if (len == 0 || len > 7 || len + 1 > dlc) return ISOTP_ERROR;
      memcpy(r.buf, d + 1, len);
      r.len = len;
      return ISOTP_DONE;
    }
    case 0x1: {                                   // First Frame
      if (dlc < 8) { r.active = false; return ISOTP_ERROR; }
      const uint16_t total = (uint16_t)((d[0] & 0x0F) << 8 | d[1]);
      if (total < 8 || total > ISOTP_MAX) { r.active = false; return ISOTP_ERROR; }
      memcpy(r.buf, d + 2, 6);
      r.total = total;
      r.len = 6;
      r.nextSn = 1;
      r.active = true;
      return ISOTP_NEED_FC;
    }
    case 0x2: {                                   // Consecutive Frame
      if (!r.active) return ISOTP_NONE;
      if ((d[0] & 0x0F) != r.nextSn) { r.active = false; return ISOTP_ERROR; }
      size_t take = (size_t)(r.total - r.len);
      if (take > 7) take = 7;
      if (take > (size_t)(dlc - 1)) take = (size_t)(dlc - 1);
      memcpy(r.buf + r.len, d + 1, take);
      r.len = (uint16_t)(r.len + take);
      r.nextSn = (uint8_t)((r.nextSn + 1) & 0x0F);
      if (r.len >= r.total) { r.active = false; return ISOTP_DONE; }
      return ISOTP_NONE;
    }
    default:                                      // Flow Control: not ours
      return ISOTP_NONE;
  }
}

// ---------------------------------------------------------------------------
// Sequencer
// ---------------------------------------------------------------------------

static inline bool reached(uint32_t now, uint32_t t) {
  return (int32_t)(now - t) >= 0;
}

static bool midSupported(const BookendSeq &s, unsigned mid) {
  return mid <= 0xFF && (s.m06Support[mid / 8] >> (mid % 8)) & 1u;
}

void bookendSeqStart(BookendSeq &s, uint8_t kind, uint32_t now,
                     uint32_t budgetMs, const BookendTiming &t) {
  memset(&s, 0, sizeof(s));
  bookendInit(s.d, kind);
  s.t = t;
  s.deadline = now + budgetMs;
  s.step = BK_S_PID01;
}

static bool gotPid01(const BookendSeq &s) {
  return !(s.d.pid01[0] == 0xFF && s.d.pid01[1] == 0xFF &&
           s.d.pid01[2] == 0xFF && s.d.pid01[3] == 0xFF);
}

static void finish(BookendSeq &s) {
  if (!s.any) s.d.flags |= BOOKEND_F_NO_RESPONSE;
  if (s.outOfTime || s.txFailed) s.d.flags |= BOOKEND_F_TRUNCATED;
  if (s.stepsDone == 4 && !(s.d.flags & BOOKEND_F_TRUNCATED))
    s.d.flags |= BOOKEND_F_COMPLETE;
  s.finished = true;
}

// A request finished normally (answered, or quiet long enough): advance.
static void stepDone(BookendSeq &s) {
  switch (s.step) {
    case BK_S_PID01: s.stepsDone++; s.step = BK_S_M03; break;
    case BK_S_M03:   s.stepsDone++; s.step = BK_S_M07; break;
    case BK_S_M07:
      s.stepsDone++;
      s.step = BK_S_M06_SUPPORT;
      s.m06Base = 0;
      break;
    case BK_S_M06_SUPPORT:
      // Bit for base+0x20 = "the next range exists".
      if (s.m06Base < 0xE0 && midSupported(s, s.m06Base + 0x20u)) {
        s.m06Base = (uint8_t)(s.m06Base + 0x20);
      } else {
        s.step = BK_S_M06_MID;
        s.m06Next = 1;
      }
      break;
    case BK_S_M06_MID:
      s.m06Next = (uint16_t)(s.reqPid + 1);
      break;
  }
}

static void request(BookendSeq &s, uint8_t mode, int pid, uint32_t now,
                    BookendTx *out) {
  memset(out, 0, sizeof(*out));
  out->id = 0x7DF;
  memset(out->data, 0x55, 8);
  if (pid >= 0) {
    out->data[0] = 0x02;
    out->data[1] = mode;
    out->data[2] = (uint8_t)pid;
  } else {
    out->data[0] = 0x01;
    out->data[1] = mode;
  }
  s.reqMode = mode;
  s.reqPid = pid >= 0 ? (uint8_t)pid : 0;
  s.inflight = true;
  s.sentMs = now;
  s.lastRxMs = now;
  memset(s.rx, 0, sizeof(s.rx));
  s.fcPending = 0;
}

BookendAct bookendSeqPoll(BookendSeq &s, uint32_t now, BookendTx *out) {
  if (s.finished) return BK_DONE;

  // Flow Control first: an ECU mid-message is waiting on it.
  if (s.fcPending) {
    uint8_t i = 0;
    while (!(s.fcPending & (1u << i))) i++;
    s.fcPending &= (uint8_t)~(1u << i);
    memset(out->data, 0x55, 8);
    out->id = 0x7E0u + i;          // the responder's physical request id
    out->data[0] = 0x30;           // CTS
    out->data[1] = 0x00;           // block size: all remaining
    out->data[2] = 0x00;           // STmin: as fast as you like
    return BK_SEND;
  }

  if (s.inflight) {
    bool assembling = false;
    for (int i = 0; i < BOOKEND_RESPONDERS; i++) assembling |= s.rx[i].active;
    const bool quiet = (uint32_t)(now - s.lastRxMs) >= s.t.quietMs && !assembling;
    const bool capped = (uint32_t)(now - s.sentMs) >= s.t.reqMaxMs;
    if (quiet || capped) {
      s.inflight = false;
      stepDone(s);
    } else if (reached(now, s.deadline)) {
      // Cut off mid-request: this read did NOT complete.
      s.inflight = false;
      s.outOfTime = true;
      finish(s);
      return BK_DONE;
    } else {
      return BK_WAIT;
    }
  }

  for (;;) {
    if (s.step == BK_S_END) { finish(s); return BK_DONE; }
    if ((int32_t)(s.deadline - now) < (int32_t)s.t.minStartMs) {
      s.outOfTime = true;
      finish(s);
      return BK_DONE;
    }
    switch (s.step) {
      case BK_S_PID01:       request(s, 0x01, 0x01, now, out); return BK_SEND;
      case BK_S_M03:         request(s, 0x03, -1, now, out);   return BK_SEND;
      case BK_S_M07:         request(s, 0x07, -1, now, out);   return BK_SEND;
      case BK_S_M06_SUPPORT: request(s, 0x06, s.m06Base, now, out); return BK_SEND;
      case BK_S_M06_MID: {
        unsigned mid = s.m06Next;
        while (mid <= 0xFF && (mid % 0x20 == 0 || !midSupported(s, mid))) mid++;
        if (mid > 0xFF) {
          s.stepsDone++;               // every supported MID read
          s.step = BK_S_END;
          continue;
        }
        request(s, 0x06, (int)mid, now, out);
        return BK_SEND;
      }
    }
  }
}

static void handleMessage(BookendSeq &s, const uint8_t *p, size_t n) {
  if (n < 1 || p[0] != (uint8_t)(s.reqMode + 0x40)) return;
  s.any = true;
  switch (s.step) {
    case BK_S_PID01:
      if (!gotPid01(s)) bookendParsePid01(s.d, p, n);   // first responder
      break;
    case BK_S_M03: bookendParseDtcs(s.d, false, p, n); break;
    case BK_S_M07: bookendParseDtcs(s.d, true, p, n); break;
    case BK_S_M06_SUPPORT:
      bookendParseM06Support(s.m06Base, p, n, s.m06Support);
      break;
    case BK_S_M06_MID:
      if (n >= 2 && p[1] == s.reqPid) bookendParseM06Results(s.d, p, n);
      break;
  }
}

void bookendSeqOnFrame(BookendSeq &s, uint32_t id, const uint8_t *data,
                       uint8_t dlc, uint32_t now) {
  if (!s.inflight || s.finished || id < 0x7E8 || id > 0x7EF) return;
  const int i = (int)(id - 0x7E8);
  s.lastRxMs = now;
  switch (isoTpFeed(s.rx[i], data, dlc)) {
    case ISOTP_NEED_FC: s.fcPending |= (uint8_t)(1u << i); break;
    case ISOTP_DONE:    handleMessage(s, s.rx[i].buf, s.rx[i].len); break;
    default: break;
  }
}

void bookendSeqTxFailed(BookendSeq &s) {
  if (s.finished) return;
  s.inflight = false;
  s.txFailed = true;
  finish(s);
}

bool bookendSeqShouldWrite(const BookendSeq &s, const char **whyNot) {
  if (whyNot) *whyNot = nullptr;
  if (s.d.kind == BOOKEND_KIND_START) return true;
  if (s.stepsDone > 0) return true;
  if (whyNot)
    *whyNot = s.txFailed ? "TX refused before any read completed"
                         : "time budget ran out before any read completed";
  return false;
}

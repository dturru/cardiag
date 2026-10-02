// bookend.h: record encoding (shared vector), parsers, ISO-TP reassembly and
// the time-budgeted sequencer against a simulated ECU.

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "bookend.h"

void setUp(void) {}
void tearDown(void) {}

// ⭐ SHARED VECTOR -- the same hex is in carhub's decoder tests.
// device_id 0x11223344, boot_id 7, ms 123456, trip_start, complete,
// pid01 81 07 65 00 (raw), stored [(ecu 0, 0x0133)],
// pending [(0, 0x0420), (1, 0xC123)], Mode 06 [(0, 01,80,0A,0123,0000,0400),
// (1, 21,87,24,8000,0010,FFFF)].
static const char kVectorHex[] =
    "03012800443322110700000040e20100010100008107650001003301020020040123c102"
    "0001800a2301000000040121872400801000ffff";

static BookendData vectorData() {
  BookendData d;
  bookendInit(d, BOOKEND_KIND_START);
  d.flags = BOOKEND_F_COMPLETE;
  const uint8_t p[4] = {0x81, 0x07, 0x65, 0x00};
  memcpy(d.pid01, p, 4);
  d.stored[d.nStored++] = {0, 0x0133};
  d.pending[d.nPending++] = {0, 0x0420};
  d.pending[d.nPending++] = {1, 0xC123};
  d.m06[d.nM06++] = {0, 0x01, 0x80, 0x0A, 0x0123, 0x0000, 0x0400};
  d.m06[d.nM06++] = {1, 0x21, 0x87, 0x24, 0x8000, 0x0010, 0xFFFF};
  return d;
}

static void hex(const uint8_t *b, size_t n, char *out) {
  for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
}

void test_shared_vector(void) {
  uint8_t buf[BOOKEND_REC_MAX];
  const size_t n = bookendEncode(vectorData(), 0x11223344, 7, 123456, buf, sizeof(buf));
  TEST_ASSERT_EQUAL(56, n);
  char h[2 * BOOKEND_REC_MAX + 1];
  hex(buf, n, h);
  TEST_ASSERT_EQUAL_STRING(kVectorHex, h);
}

void test_empty_record_and_no_response_pid01(void) {
  BookendData d;
  bookendInit(d, BOOKEND_KIND_END);
  d.flags = BOOKEND_F_NO_RESPONSE;
  uint8_t buf[64];
  const size_t n = bookendEncode(d, 1, 2, 3, buf, sizeof(buf));
  TEST_ASSERT_EQUAL(BOOKEND_REC_HDR + 11, n);
  TEST_ASSERT_EQUAL_HEX8(0x03, buf[0]);
  TEST_ASSERT_EQUAL_HEX8(11, buf[2]);                 // payload_len
  TEST_ASSERT_EQUAL_HEX8(BOOKEND_KIND_END, buf[16]);
  TEST_ASSERT_EQUAL_HEX8(0x04, buf[17]);
  for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL_HEX8(0xFF, buf[20 + i]);
  TEST_ASSERT_EQUAL(0, bookendEncode(d, 1, 2, 3, buf, n - 1));  // too small
}

void test_max_record_fits(void) {
  BookendData d;
  bookendInit(d, BOOKEND_KIND_START);
  d.nStored = BOOKEND_MAX_STORED;
  d.nPending = BOOKEND_MAX_PENDING;
  d.nM06 = BOOKEND_MAX_M06;
  static uint8_t buf[BOOKEND_REC_MAX];
  TEST_ASSERT_EQUAL(BOOKEND_REC_MAX, bookendEncode(d, 0, 0, 0, buf, sizeof(buf)));
}

void test_file_header(void) {
  uint8_t h[BOOKEND_FILE_HDR];
  bookendFileHeader(h, 3, 0x11223344, 7);
  TEST_ASSERT_EQUAL_MEMORY("CDGB", h, 4);
  TEST_ASSERT_EQUAL_HEX8(1, h[4]);
  TEST_ASSERT_EQUAL_HEX8(3, h[5]);
  TEST_ASSERT_EQUAL_HEX8(0x44, h[8]);
  TEST_ASSERT_EQUAL_HEX8(7, h[12]);
}

// --- parsers -------------------------------------------------------------------

void test_dtc_parse_count_padding_and_cap(void) {
  BookendData d;
  bookendInit(d, BOOKEND_KIND_START);
  const uint8_t m03[] = {0x43, 0x03, 0x01, 0x33, 0x00, 0x00, 0xC1, 0x23};
  TEST_ASSERT_TRUE(bookendParseDtcs(d, 2, false, m03, sizeof(m03)));
  TEST_ASSERT_EQUAL_UINT8(2, d.nStored);              // 0x0000 skipped
  TEST_ASSERT_EQUAL_HEX16(0x0133, d.stored[0].dtc);
  TEST_ASSERT_EQUAL_UINT8(2, d.stored[0].ecu);
  TEST_ASSERT_EQUAL_HEX16(0xC123, d.stored[1].dtc);
  // Count larger than the bytes present: trust the bytes.
  const uint8_t lying[] = {0x47, 0x09, 0x04, 0x20};
  TEST_ASSERT_TRUE(bookendParseDtcs(d, 0, true, lying, sizeof(lying)));
  TEST_ASSERT_EQUAL_UINT8(1, d.nPending);
  TEST_ASSERT_FALSE(bookendParseDtcs(d, 0, true, m03, sizeof(m03)));  // wrong mode
  // Cap.
  uint8_t many[2 + 2 * 40];
  many[0] = 0x43; many[1] = 40;
  for (int i = 0; i < 40; i++) { many[2 + 2 * i] = 0x01; many[3 + 2 * i] = (uint8_t)(i + 1); }
  BookendData e;
  bookendInit(e, BOOKEND_KIND_START);
  bookendParseDtcs(e, 0, false, many, sizeof(many));
  TEST_ASSERT_EQUAL_UINT8(BOOKEND_MAX_STORED, e.nStored);
  TEST_ASSERT_TRUE(e.flags & BOOKEND_F_TRUNCATED);
}

void test_m06_support_and_results(void) {
  uint8_t sup[32] = {0};
  const uint8_t s0[] = {0x46, 0x00, 0xC0, 0x00, 0x00, 0x01};   // MIDs 01, 02, 20
  TEST_ASSERT_TRUE(bookendParseM06Support(0x00, s0, sizeof(s0), sup));
  TEST_ASSERT_TRUE(sup[0] & 0x02);                    // MID 01
  TEST_ASSERT_TRUE(sup[0] & 0x04);                    // MID 02
  TEST_ASSERT_TRUE(sup[4] & 0x01);                    // MID 0x20
  TEST_ASSERT_FALSE(bookendParseM06Support(0x20, s0, sizeof(s0), sup));

  BookendData d;
  bookendInit(d, BOOKEND_KIND_START);
  const uint8_t r[] = {0x46, 0x01, 0x80, 0x0A, 0x01, 0x23, 0x00, 0x00, 0x04, 0x00,
                       0x01, 0x81, 0x0A, 0x00, 0x10, 0x00, 0x00, 0x00, 0x20, 0x99};
  TEST_ASSERT_TRUE(bookendParseM06Results(d, 3, r, sizeof(r)));
  TEST_ASSERT_EQUAL_UINT8(3, d.m06[0].ecu);
  TEST_ASSERT_EQUAL_UINT8(2, d.nM06);                 // trailing partial group dropped
  TEST_ASSERT_EQUAL_HEX16(0x0123, d.m06[0].value);
  TEST_ASSERT_EQUAL_HEX16(0x0400, d.m06[0].max);
  TEST_ASSERT_EQUAL_HEX8(0x81, d.m06[1].tid);
}

// --- ISO-TP ----------------------------------------------------------------------

void test_isotp_single_and_multi(void) {
  IsoTpRx r = {};
  const uint8_t sf[8] = {0x06, 0x41, 0x01, 0x81, 0x07, 0x65, 0x00, 0x55};
  TEST_ASSERT_EQUAL(ISOTP_DONE, isoTpFeed(r, sf, 8));
  TEST_ASSERT_EQUAL_UINT16(6, r.len);

  const uint8_t ff[8]  = {0x10, 0x0A, 0x43, 0x04, 0x01, 0x33, 0x01, 0x34};
  const uint8_t cf1[8] = {0x21, 0x01, 0x35, 0x01, 0x36, 0x55, 0x55, 0x55};
  TEST_ASSERT_EQUAL(ISOTP_NEED_FC, isoTpFeed(r, ff, 8));
  TEST_ASSERT_EQUAL(ISOTP_DONE, isoTpFeed(r, cf1, 8));
  TEST_ASSERT_EQUAL_UINT16(10, r.len);
  TEST_ASSERT_EQUAL_HEX8(0x36, r.buf[9]);
}

void test_isotp_rejects_bad_sequence_and_oversize(void) {
  IsoTpRx r = {};
  const uint8_t ff[8]  = {0x10, 0x14, 1, 2, 3, 4, 5, 6};
  const uint8_t cf2[8] = {0x22, 0, 0, 0, 0, 0, 0, 0};
  isoTpFeed(r, ff, 8);
  TEST_ASSERT_EQUAL(ISOTP_ERROR, isoTpFeed(r, cf2, 8));
  TEST_ASSERT_FALSE(r.active);
  const uint8_t huge[8] = {0x1F, 0xFF, 1, 2, 3, 4, 5, 6};
  TEST_ASSERT_EQUAL(ISOTP_ERROR, isoTpFeed(r, huge, 8));
  const uint8_t stray[8] = {0x21, 0, 0, 0, 0, 0, 0, 0};
  TEST_ASSERT_EQUAL(ISOTP_NONE, isoTpFeed(r, stray, 8));     // no FF: ignored
}

// --- sequencer against a simulated ECU ---------------------------------------

static const BookendTiming kT = {50, 300, 80};

struct Ecu {
  // Pending frames to deliver: (at, data)
  struct Q { uint32_t at; uint8_t d[8]; } q[64];
  int n;
  int latencyMs;
  bool silent;
  uint8_t mfPending[64];       // remaining CFs after an FF
  int mfLen;
  int fcSeen;
  int requests;
};

static void push(Ecu &e, uint32_t at, const uint8_t *d) {
  e.q[e.n].at = at;
  memcpy(e.q[e.n].d, d, 8);
  e.n++;
}

// The ECU's canned answers. Mode 03 is multi-frame (3 DTCs) to exercise FC.
static void respond(Ecu &e, const BookendTx &tx, uint32_t now) {
  if (e.silent) return;
  const uint32_t at = now + (uint32_t)e.latencyMs;
  if (tx.id >= 0x7E0 && tx.id <= 0x7E7) {             // Flow Control
    e.fcSeen++;
    const uint8_t cf[8] = {0x21, 0x01, 0x35, 0x55, 0x55, 0x55, 0x55, 0x55};
    push(e, at, cf);
    return;
  }
  e.requests++;
  const uint8_t mode = tx.data[1], pid = tx.data[2];
  if (mode == 0x01 && pid == 0x01) {
    const uint8_t f[8] = {0x06, 0x41, 0x01, 0x83, 0x07, 0x65, 0x00, 0x55};
    push(e, at, f);
  } else if (mode == 0x03) {
    const uint8_t ff[8] = {0x10, 0x08, 0x43, 0x03, 0x01, 0x33, 0x01, 0x34};
    push(e, at, ff);
  } else if (mode == 0x07) {
    const uint8_t f[8] = {0x02, 0x47, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55};
    push(e, at, f);
  } else if (mode == 0x06 && pid == 0x00) {
    const uint8_t f[8] = {0x06, 0x46, 0x00, 0x80, 0x00, 0x00, 0x00, 0x55};  // MID 01 only
    push(e, at, f);
  } else if (mode == 0x06 && pid == 0x01) {
    // One result = 10 bytes -> multi-frame.
    const uint8_t ff[8] = {0x10, 0x0A, 0x46, 0x01, 0x80, 0x0A, 0x01, 0x23};
    push(e, at, ff);
    // CF content for this one differs from the 03 one; patch on FC below.
  }
}

// Run the sequencer to completion; returns ms elapsed.
static uint32_t run(BookendSeq &s, Ecu &e, uint32_t t0, uint32_t budget,
                    uint8_t kind = BOOKEND_KIND_START) {
  bookendSeqStart(s, kind, t0, budget, kT);
  uint8_t lastMode = 0;
  for (uint32_t now = t0; now < t0 + 10000; now++) {
    for (int i = 0; i < e.n;) {
      if (e.q[i].at <= now) {
        uint8_t d[8];
        memcpy(d, e.q[i].d, 8);
        // Mode 06 MID 01 continuation frame.
        if (d[0] == 0x21 && lastMode == 0x06) {
          const uint8_t cf[8] = {0x21, 0x00, 0x00, 0x04, 0x00, 0x55, 0x55, 0x55};
          memcpy(d, cf, 8);
        }
        bookendSeqOnFrame(s, 0x7E8, d, 8, now);
        e.q[i] = e.q[--e.n];
      } else {
        i++;
      }
    }
    BookendTx tx;
    const BookendAct a = bookendSeqPoll(s, now, &tx);
    if (a == BK_DONE) return now - t0;
    if (a == BK_SEND) {
      if (tx.id == 0x7DF) lastMode = tx.data[1];
      respond(e, tx, now);
    }
  }
  return 10000;
}

void test_full_bookend_with_multiframe(void) {
  static BookendSeq s;
  Ecu e = {};
  e.latencyMs = 10;
  run(s, e, 1000, 5000);
  TEST_ASSERT_TRUE(s.finished);
  TEST_ASSERT_EQUAL_HEX8(BOOKEND_F_COMPLETE, s.d.flags);
  TEST_ASSERT_EQUAL_HEX8(0x83, s.d.pid01[0]);
  TEST_ASSERT_EQUAL_UINT8(3, s.d.nStored);             // via FF + CF + FC
  TEST_ASSERT_EQUAL_HEX16(0x0135, s.d.stored[2].dtc);
  TEST_ASSERT_EQUAL_UINT8(0, s.d.nPending);
  TEST_ASSERT_EQUAL_UINT8(1, s.d.nM06);
  TEST_ASSERT_EQUAL_HEX16(0x0400, s.d.m06[0].max);
  TEST_ASSERT_EQUAL(2, e.fcSeen);
  TEST_ASSERT_EQUAL(5, e.requests);                    // 01/01, 03, 07, 06/00, 06/01
  TEST_ASSERT_TRUE(bookendSeqShouldWrite(s, nullptr));
}

void test_no_response_still_ends_and_flags(void) {
  static BookendSeq s;
  Ecu e = {};
  e.silent = true;
  const uint32_t took = run(s, e, 0, 5000);
  TEST_ASSERT_TRUE(s.finished);
  TEST_ASSERT_TRUE(s.d.flags & BOOKEND_F_NO_RESPONSE);
  TEST_ASSERT_TRUE(s.d.flags & BOOKEND_F_COMPLETE);    // every read finished, all silent
  TEST_ASSERT_TRUE(took < 400);                        // 4 x quietMs, no waiting on caps
}

// ⭐ BUDGET: the sequence never runs past its deadline.
void test_budget_is_a_hard_deadline(void) {
  static BookendSeq s;
  Ecu e = {};
  e.latencyMs = 40;                                    // slow ECU
  const uint32_t took = run(s, e, 0, 200, BOOKEND_KIND_END);
  TEST_ASSERT_TRUE(took <= 200);
  TEST_ASSERT_TRUE(s.d.flags & BOOKEND_F_TRUNCATED);
  TEST_ASSERT_FALSE(s.d.flags & BOOKEND_F_COMPLETE);
  TEST_ASSERT_TRUE(s.stepsDone >= 1);                  // something was read
  TEST_ASSERT_TRUE(bookendSeqShouldWrite(s, nullptr)); // partial END is written
}

void test_end_with_no_completed_read_is_skipped(void) {
  static BookendSeq s;
  Ecu e = {};
  e.latencyMs = 10;
  run(s, e, 0, 60, BOOKEND_KIND_END);                  // less than minStart+quiet
  TEST_ASSERT_EQUAL_UINT8(0, s.stepsDone);
  const char *why = nullptr;
  TEST_ASSERT_FALSE(bookendSeqShouldWrite(s, &why));
  TEST_ASSERT_EQUAL_STRING("time budget ran out before any read completed", why);
  // The same budget for a START still writes (flags say how little it got).
  run(s, e, 0, 60, BOOKEND_KIND_START);
  TEST_ASSERT_TRUE(bookendSeqShouldWrite(s, nullptr));
}

void test_budget_too_small_to_start_sends_nothing(void) {
  static BookendSeq s;
  bookendSeqStart(s, BOOKEND_KIND_END, 0, 50, kT);     // < minStartMs
  BookendTx tx;
  TEST_ASSERT_EQUAL(BK_DONE, bookendSeqPoll(s, 0, &tx));
  TEST_ASSERT_TRUE(s.outOfTime);
}

void test_tx_refused_ends_sequence(void) {
  static BookendSeq s;
  bookendSeqStart(s, BOOKEND_KIND_END, 0, 2000, kT);
  BookendTx tx;
  TEST_ASSERT_EQUAL(BK_SEND, bookendSeqPoll(s, 0, &tx));
  bookendSeqTxFailed(s);
  TEST_ASSERT_EQUAL(BK_DONE, bookendSeqPoll(s, 1, &tx));
  const char *why = nullptr;
  TEST_ASSERT_FALSE(bookendSeqShouldWrite(s, &why));
  TEST_ASSERT_EQUAL_STRING("TX refused before any read completed", why);
}

void test_two_ecus_dtcs_accumulate(void) {
  static BookendSeq s;
  bookendSeqStart(s, BOOKEND_KIND_START, 0, 5000, kT);
  BookendTx tx;
  bookendSeqPoll(s, 0, &tx);                           // 01/01
  for (uint32_t t = 1; bookendSeqPoll(s, t, &tx) != BK_SEND; t++) {}   // -> 03 after quiet
  TEST_ASSERT_EQUAL_HEX8(0x03, tx.data[1]);
  const uint8_t a[8] = {0x04, 0x43, 0x01, 0x01, 0x33, 0x55, 0x55, 0x55};
  const uint8_t b[8] = {0x04, 0x43, 0x01, 0x07, 0x00, 0x55, 0x55, 0x55};
  bookendSeqOnFrame(s, 0x7E8, a, 8, 60);
  bookendSeqOnFrame(s, 0x7E9, b, 8, 61);
  bookendSeqOnFrame(s, 0x123, a, 8, 62);               // not an OBD responder
  TEST_ASSERT_EQUAL_UINT8(2, s.d.nStored);
  TEST_ASSERT_EQUAL_HEX16(0x0700, s.d.stored[1].dtc);
  TEST_ASSERT_EQUAL_UINT8(1, s.d.stored[1].ecu);
}

// The same DTC from two ECUs is two entries: not de-duplicated.
void test_same_dtc_from_two_ecus_kept_twice(void) {
  static BookendSeq s;
  bookendSeqStart(s, BOOKEND_KIND_START, 0, 5000, kT);
  BookendTx tx;
  bookendSeqPoll(s, 0, &tx);
  for (uint32_t t = 1; bookendSeqPoll(s, t, &tx) != BK_SEND; t++) {}
  const uint8_t a[8] = {0x04, 0x43, 0x01, 0x01, 0x33, 0x55, 0x55, 0x55};
  bookendSeqOnFrame(s, 0x7E8, a, 8, 60);
  bookendSeqOnFrame(s, 0x7EA, a, 8, 61);
  TEST_ASSERT_EQUAL_UINT8(2, s.d.nStored);
  TEST_ASSERT_EQUAL_UINT8(0, s.d.stored[0].ecu);
  TEST_ASSERT_EQUAL_UINT8(2, s.d.stored[1].ecu);
}

// PID 01 from ECU 1 only: pid01 stays 0xFF and no-response is set, even
// though something answered.
void test_pid01_only_from_ecu0(void) {
  static BookendSeq s;
  bookendSeqStart(s, BOOKEND_KIND_START, 0, 5000, kT);
  BookendTx tx;
  bookendSeqPoll(s, 0, &tx);                           // 01/01
  const uint8_t f[8] = {0x06, 0x41, 0x01, 0x80, 0x00, 0x00, 0x00, 0x55};
  bookendSeqOnFrame(s, 0x7E9, f, 8, 10);
  for (uint32_t t = 11; t < 5000 && bookendSeqPoll(s, t, &tx) != BK_DONE; t++) {}
  for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL_HEX8(0xFF, s.d.pid01[i]);
  TEST_ASSERT_TRUE(s.d.flags & BOOKEND_F_NO_RESPONSE);
  // And from ECU 0 it is taken.
  bookendSeqStart(s, BOOKEND_KIND_START, 0, 5000, kT);
  bookendSeqPoll(s, 0, &tx);
  bookendSeqOnFrame(s, 0x7E9, f, 8, 10);
  const uint8_t g[8] = {0x06, 0x41, 0x01, 0x83, 0x07, 0x65, 0x00, 0x55};
  bookendSeqOnFrame(s, 0x7E8, g, 8, 11);
  TEST_ASSERT_EQUAL_HEX8(0x83, s.d.pid01[0]);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_shared_vector);
  RUN_TEST(test_empty_record_and_no_response_pid01);
  RUN_TEST(test_max_record_fits);
  RUN_TEST(test_file_header);
  RUN_TEST(test_dtc_parse_count_padding_and_cap);
  RUN_TEST(test_m06_support_and_results);
  RUN_TEST(test_isotp_single_and_multi);
  RUN_TEST(test_isotp_rejects_bad_sequence_and_oversize);
  RUN_TEST(test_full_bookend_with_multiframe);
  RUN_TEST(test_no_response_still_ends_and_flags);
  RUN_TEST(test_budget_is_a_hard_deadline);
  RUN_TEST(test_end_with_no_completed_read_is_skipped);
  RUN_TEST(test_budget_too_small_to_start_sends_nothing);
  RUN_TEST(test_tx_refused_ends_sequence);
  RUN_TEST(test_two_ecus_dtcs_accumulate);
  RUN_TEST(test_same_dtc_from_two_ecus_kept_twice);
  RUN_TEST(test_pid01_only_from_ecu0);
  return UNITY_END();
}

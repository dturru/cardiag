#pragma once

// ---------------------------------------------------------------------------
// TIER C TRIP BOOKENDS. At trip start and trip end the logger reads
//   Mode 01 PID 01   MIL + readiness (bytes A-D)
//   Mode 03          stored DTCs
//   Mode 07          pending DTCs
//   Mode 06          on-board monitoring results for every supported MID
// and writes one bookend record per event. Raw: the logger does not decode
// (rule 3); the hub turns DTCs into P/C/B/U text, MIL/readiness into flags and
// Mode 06 values into units via UASID.
//
// Pure, no Arduino/FreeRTOS. test/test_bookend drives the encoder (against the
// shared vector in carhub), ISO-TP reassembly, the reply parsers and the
// time-budgeted sequencer with a simulated ECU. poller.cpp owns the CAN side.
//
// RECORD (protocol v1 rec_type 3, file kind `bookend`, format `cdgb1`),
// little-endian:
//   header, 16 B   u8 rec_type=3 | u8 version=1 | u16 payload_len
//                  | u32 device_id | u32 boot_id | u32 ms
//   payload        u8 kind (1 trip_start, 2 trip_end) | u8 flags | u16 rsv=0
//                  | u8[4] pid01_raw   from ECU 0 (0x7E8) only; all 0xFF
//                                     and bit2 set if ECU 0 did not answer
//                  | u8 n_stored  | n_stored  x {u8 ecu, u16 dtc}   (Mode 03)
//                  | u8 n_pending | n_pending x {u8 ecu, u16 dtc}   (Mode 07)
//                  | u8 n_m06     | n_m06 x {u8 ecu, u8 mid, u8 tid, u8 uasid,
//                                            u16 value, u16 min, u16 max}
//   flags          bit0 complete | bit1 truncated (time budget OR a cap)
//                  | bit2 no-response (ECU 0 gave no Mode 01 PID 01 reply)
// ecu = response CAN id - 0x7E8 (0 = engine). Entries are NOT de-duplicated
// across ECUs. A DTC u16 is the two bytes as received, high byte first on the
// wire: dtc = b0 << 8 | b1, then stored little-endian like every other u16.
// The file starts with a 16 B header like CDGS:
//   "CDGB" | u8 version=1 | u8 rec_type=3 | u8 mode | u8 rsv | u32 device_id
//   | u32 boot_id
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#define HUB_REC_BOOKEND        3
#define BOOKEND_VERSION        1
#define BOOKEND_KIND_START     1
#define BOOKEND_KIND_END       2
#define BOOKEND_F_COMPLETE     0x01
#define BOOKEND_F_TRUNCATED    0x02
#define BOOKEND_F_NO_RESPONSE  0x04

#define BOOKEND_MAX_STORED     32
#define BOOKEND_MAX_PENDING    32
#define BOOKEND_MAX_M06        64

#define BOOKEND_REC_HDR        16
#define BOOKEND_FILE_HDR       16
#define BOOKEND_DTC_LEN        3     // u8 ecu + u16 dtc
#define BOOKEND_M06_LEN        10    // u8 ecu + mid,tid,uasid + 3 x u16
#define BOOKEND_PAYLOAD_MAX    (4 + 4 + 1 + BOOKEND_DTC_LEN * BOOKEND_MAX_STORED + \
                                1 + BOOKEND_DTC_LEN * BOOKEND_MAX_PENDING + 1 + \
                                BOOKEND_M06_LEN * BOOKEND_MAX_M06)
#define BOOKEND_REC_MAX        (BOOKEND_REC_HDR + BOOKEND_PAYLOAD_MAX)

struct BookendDtc {
  uint8_t  ecu;
  uint16_t dtc;
};

struct BookendM06 {
  uint8_t  ecu;
  uint8_t  mid, tid, uasid;
  uint16_t value, min, max;
};

struct BookendData {
  uint8_t    kind;
  uint8_t    flags;
  uint8_t    pid01[4];
  uint8_t    nStored;
  BookendDtc stored[BOOKEND_MAX_STORED];
  uint8_t    nPending;
  BookendDtc pending[BOOKEND_MAX_PENDING];
  uint8_t    nM06;
  BookendM06 m06[BOOKEND_MAX_M06];
};

void bookendInit(BookendData &d, uint8_t kind);

// Encode one record (header + payload). Returns bytes written, 0 if cap is
// too small.
size_t bookendEncode(const BookendData &d, uint32_t deviceId, uint32_t bootId,
                     uint32_t ms, uint8_t *buf, size_t cap);
void bookendFileHeader(uint8_t out[BOOKEND_FILE_HDR], uint8_t mode,
                       uint32_t deviceId, uint32_t bootId);

// ---------------------------------------------------------------------------
// Reply parsers. `p` is the reassembled ISO-TP payload (starting at the reply
// mode byte, 0x41/0x43/0x47/0x46) from responder `ecu` (CAN id - 0x7E8). Each
// appends to `d` and sets the truncated flag when a cap is hit.
// ---------------------------------------------------------------------------
bool bookendParsePid01(BookendData &d, const uint8_t *p, size_t n);
bool bookendParseDtcs(BookendData &d, uint8_t ecu, bool pending,
                      const uint8_t *p, size_t n);
// Mode 06 MID 00/20/40/... bitmap: sets bits for MIDs base+1..base+32 in
// `supported` (256 bits). Returns false if not a bitmap reply for `base`.
bool bookendParseM06Support(uint8_t base, const uint8_t *p, size_t n,
                            uint8_t supported[32]);
bool bookendParseM06Results(BookendData &d, uint8_t ecu, const uint8_t *p,
                            size_t n);

// ---------------------------------------------------------------------------
// ISO-TP receive (ISO 15765-2), one per responder. Enough for OBD replies:
// SF, FF + CF, sequence check, a size cap.
// ---------------------------------------------------------------------------
#define ISOTP_MAX 160

enum IsoTpEv : uint8_t {
  ISOTP_NONE = 0,      // frame consumed, nothing complete yet
  ISOTP_DONE,          // a full message is in buf[0..len)
  ISOTP_NEED_FC,       // First Frame: send Flow Control to the responder now
  ISOTP_ERROR,         // bad sequence / oversize: message dropped
};

struct IsoTpRx {
  bool     active;     // assembling a multi-frame message
  uint16_t total;
  uint16_t len;
  uint8_t  nextSn;
  uint8_t  buf[ISOTP_MAX];
};

IsoTpEv isoTpFeed(IsoTpRx &r, const uint8_t *data, uint8_t dlc);

// ---------------------------------------------------------------------------
// Sequencer. Non-blocking: the caller polls it for the next action and feeds
// it every frame from 0x7E8..0x7EF. One request in flight. Hard deadline.
// ---------------------------------------------------------------------------
#define BOOKEND_RESPONDERS 8          // 0x7E8..0x7EF

struct BookendTiming {
  uint32_t quietMs;     // a request is answered once this long passes with
                        // no frame and nothing mid-assembly
  uint32_t reqMaxMs;    // hard cap per request
  uint32_t minStartMs;  // do not START a request with less than this left
};

enum BookendAct : uint8_t {
  BK_WAIT = 0,          // nothing to send now
  BK_SEND,              // send *out (a request or a Flow Control)
  BK_DONE,              // finished; see bookendSeqOutcome
};

struct BookendTx {
  uint32_t id;
  uint8_t  data[8];
};

enum BookendStep : uint8_t {
  BK_S_PID01 = 0, BK_S_M03, BK_S_M07, BK_S_M06_SUPPORT, BK_S_M06_MID, BK_S_END,
};

struct BookendSeq {
  BookendData   d;
  BookendTiming t;
  uint32_t      deadline;
  uint8_t       step;
  bool          inflight;
  uint8_t       reqMode, reqPid;
  uint32_t      sentMs, lastRxMs;
  IsoTpRx       rx[BOOKEND_RESPONDERS];
  uint8_t       fcPending;        // bitmask of responders owed a Flow Control
  uint8_t       m06Support[32];   // MIDs 0x00..0xFF
  uint16_t      m06Next;          // next MID to consider
  uint8_t       m06Base;          // support range being walked
  bool          any;              // anything at all answered
  bool          pid01Ecu0;        // ECU 0 answered Mode 01 PID 01
  uint8_t       stepsDone;        // reads completed (0..4)
  bool          outOfTime;
  bool          txFailed;
  bool          finished;
};

void bookendSeqStart(BookendSeq &s, uint8_t kind, uint32_t now,
                     uint32_t budgetMs, const BookendTiming &t);
BookendAct bookendSeqPoll(BookendSeq &s, uint32_t now, BookendTx *out);
// Offer one received frame (any id; non-OBD ids are ignored).
void bookendSeqOnFrame(BookendSeq &s, uint32_t id, const uint8_t *data,
                       uint8_t dlc, uint32_t now);
// The caller could not transmit (gate shut, driver busy): the sequence ends
// as out of time -- whatever was read is kept.
void bookendSeqTxFailed(BookendSeq &s);

// After BK_DONE: whether to write the record, and why not.
// A START is written whatever it got (flags say how much). An END with no
// completed read is SKIPPED: an empty trip-end record would read as "no DTCs
// at key-off", which is a claim nothing supports.
bool bookendSeqShouldWrite(const BookendSeq &s, const char **whyNot);

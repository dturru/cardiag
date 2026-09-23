// Host tests for the UDP packet serialiser. Run with:
//
//     pio test -e native
//
// WHY THESE EXIST
//
// The >64-record split in hubChunkRows() is currently UNREACHABLE on the
// device: SNIFF_MAX_IDS (64) equals HUB_MAX_RECORDS (64), so a snapshot cannot
// produce a 65th row. Unreachable is not the same as correct, and the table
// will grow -- a BMW has more ids than a Civic. Without these tests the first
// execution of that loop would be on a live vehicle bus.
//
// Feeding it synthetic rows on the host is the cheap way to make the path real
// before it matters. Nothing here needs a board, a hub or a network.

#include <unity.h>
#include <string.h>
#include <vector>

#include "hubchunk.h"

// ---------------------------------------------------------------------------
// Capture harness: records every packet emit() is handed.
// ---------------------------------------------------------------------------

struct Captured {
  std::vector<std::vector<uint8_t>> packets;
  int failAfter = -1;          // -1 = never fail
};

static bool capture(const uint8_t *pkt, size_t len, void *ctx) {
  Captured *c = (Captured *)ctx;
  if (c->failAfter >= 0 && (int)c->packets.size() >= c->failAfter) return false;
  c->packets.emplace_back(pkt, pkt + len);
  return true;
}

static std::vector<SnifferRow> makeRows(uint16_t n) {
  std::vector<SnifferRow> rows(n);
  for (uint16_t i = 0; i < n; i++) {
    memset(&rows[i], 0, sizeof(SnifferRow));
    // Ids deliberately span the 11-bit boundary so a truncation to uint16_t
    // anywhere in the chain would show up as a wrong id rather than as a
    // wrong count.
    rows[i].id = 0x100u + i;
    rows[i].lastMs = 1000u + i;
    rows[i].dlc = 8;
    rows[i].changedMask = (uint8_t)(i & 0xFF);
    rows[i].ext = false;
    for (uint8_t b = 0; b < 8; b++) rows[i].data[b] = (uint8_t)(i + b);
  }
  return rows;
}

static const HubHeader *hdr(const std::vector<uint8_t> &p) {
  return (const HubHeader *)p.data();
}

static const HubCanRecord *rec(const std::vector<uint8_t> &p, uint16_t i) {
  return (const HubCanRecord *)(p.data() + HUB_HEADER_LEN) + i;
}

static uint8_t g_buf[HUB_MAX_PACKET];

// ---------------------------------------------------------------------------
// The single-packet cases, so a split failure is distinguishable from a
// serialisation failure.
// ---------------------------------------------------------------------------

void test_zero_rows_emits_nothing_and_burns_no_seq(void) {
  Captured c;
  uint32_t seq = 7;
  auto rows = makeRows(1);
  HubChunkResult r = hubChunkRows(g_buf, sizeof(g_buf), rows.data(), 0,
                                  0xAABBCCDD, 3, &seq, 500, 0, capture, &c);
  TEST_ASSERT_EQUAL_UINT16(0, r.packets);
  TEST_ASSERT_EQUAL_size_t(0, c.packets.size());
  // An empty datagram would make the hub's gap detector report a loss that
  // never happened.
  TEST_ASSERT_EQUAL_UINT32(7, seq);
}

void test_one_row_is_one_packet(void) {
  Captured c;
  uint32_t seq = 0;
  auto rows = makeRows(1);
  HubChunkResult r = hubChunkRows(g_buf, sizeof(g_buf), rows.data(), 1,
                                  0xAABBCCDD, 3, &seq, 500, 0, capture, &c);
  TEST_ASSERT_EQUAL_UINT16(1, r.packets);
  TEST_ASSERT_EQUAL_UINT16(1, r.records);
  TEST_ASSERT_EQUAL_size_t(HUB_HEADER_LEN + HUB_RECORD_LEN, c.packets[0].size());
  TEST_ASSERT_EQUAL_UINT16(1, hdr(c.packets[0])->count);
}

void test_exactly_64_rows_is_still_one_packet(void) {
  Captured c;
  uint32_t seq = 0;
  auto rows = makeRows(HUB_MAX_RECORDS);
  HubChunkResult r = hubChunkRows(g_buf, sizeof(g_buf), rows.data(),
                                  HUB_MAX_RECORDS, 0xAABBCCDD, 3, &seq, 500,
                                  0, capture, &c);
  TEST_ASSERT_EQUAL_UINT16(1, r.packets);
  TEST_ASSERT_EQUAL_size_t(HUB_MAX_PACKET, c.packets[0].size());
  TEST_ASSERT_EQUAL_UINT32(1, seq);
}

// ---------------------------------------------------------------------------
// THE SPLIT. This is what the module was extracted for.
// ---------------------------------------------------------------------------

void test_65_rows_splits_into_64_plus_1(void) {
  Captured c;
  uint32_t seq = 0;
  auto rows = makeRows(65);
  HubChunkResult r = hubChunkRows(g_buf, sizeof(g_buf), rows.data(), 65,
                                  0xAABBCCDD, 3, &seq, 500, 0, capture, &c);
  TEST_ASSERT_EQUAL_UINT16(2, r.packets);
  TEST_ASSERT_EQUAL_UINT16(65, r.records);
  TEST_ASSERT_EQUAL_size_t(2, c.packets.size());
  TEST_ASSERT_EQUAL_UINT16(64, hdr(c.packets[0])->count);
  TEST_ASSERT_EQUAL_UINT16(1, hdr(c.packets[1])->count);
}

void test_129_rows_splits_into_64_64_1(void) {
  Captured c;
  uint32_t seq = 0;
  auto rows = makeRows(129);
  HubChunkResult r = hubChunkRows(g_buf, sizeof(g_buf), rows.data(), 129,
                                  0xAABBCCDD, 3, &seq, 500, 0, capture, &c);
  TEST_ASSERT_EQUAL_UINT16(3, r.packets);
  TEST_ASSERT_EQUAL_UINT16(129, r.records);
  TEST_ASSERT_EQUAL_UINT16(64, hdr(c.packets[0])->count);
  TEST_ASSERT_EQUAL_UINT16(64, hdr(c.packets[1])->count);
  TEST_ASSERT_EQUAL_UINT16(1, hdr(c.packets[2])->count);
}

void test_no_packet_ever_exceeds_the_mtu_budget(void) {
  Captured c;
  uint32_t seq = 0;
  auto rows = makeRows(200);
  hubChunkRows(g_buf, sizeof(g_buf), rows.data(), 200,
               0xAABBCCDD, 3, &seq, 500, 0, capture, &c);
  for (const auto &p : c.packets) {
    TEST_ASSERT_TRUE(p.size() <= (size_t)HUB_MAX_PACKET);
    // Protocol 1.2: count * record_len must equal the remaining payload.
    const uint16_t count = hdr(p)->count;
    TEST_ASSERT_EQUAL_size_t(p.size() - HUB_HEADER_LEN,
                             (size_t)count * HUB_RECORD_LEN);
  }
}

void test_seq_increments_per_packet_not_per_call(void) {
  Captured c;
  uint32_t seq = 100;
  auto rows = makeRows(129);
  hubChunkRows(g_buf, sizeof(g_buf), rows.data(), 129,
               0xAABBCCDD, 3, &seq, 500, 0, capture, &c);
  // A split that reused one seq would make a lost second half invisible to
  // the hub's gap detector -- the exact failure the counter exists to catch.
  TEST_ASSERT_EQUAL_UINT32(100, hdr(c.packets[0])->seq);
  TEST_ASSERT_EQUAL_UINT32(101, hdr(c.packets[1])->seq);
  TEST_ASSERT_EQUAL_UINT32(102, hdr(c.packets[2])->seq);
  TEST_ASSERT_EQUAL_UINT32(103, seq);
}

void test_every_row_appears_exactly_once_and_in_order(void) {
  Captured c;
  uint32_t seq = 0;
  auto rows = makeRows(150);
  hubChunkRows(g_buf, sizeof(g_buf), rows.data(), 150,
               0xAABBCCDD, 3, &seq, 500, 0, capture, &c);

  // Flatten and compare against the input. A split that dropped or duplicated
  // a boundary row is the failure mode here, and only a full comparison finds
  // it -- counts alone would pass.
  std::vector<uint32_t> seen;
  for (const auto &p : c.packets) {
    for (uint16_t i = 0; i < hdr(p)->count; i++) seen.push_back(rec(p, i)->can_id);
  }
  TEST_ASSERT_EQUAL_size_t(150, seen.size());
  for (uint16_t i = 0; i < 150; i++) {
    TEST_ASSERT_EQUAL_UINT32(0x100u + i, seen[i]);
  }
}

void test_full_snapshot_flag_rides_on_every_packet_of_a_split(void) {
  Captured c;
  uint32_t seq = 0;
  auto rows = makeRows(130);
  hubChunkRows(g_buf, sizeof(g_buf), rows.data(), 130, 0xAABBCCDD, 3, &seq,
               500, HUB_FLAG_FULL_SNAPSHOT, capture, &c);
  TEST_ASSERT_EQUAL_size_t(3, c.packets.size());
  // FULL_SNAPSHOT describes the SET. On only the first packet, the hub would
  // treat packets 2..N as incremental and never learn they were part of the
  // same full picture.
  for (const auto &p : c.packets) {
    TEST_ASSERT_TRUE(hdr(p)->flags & HUB_FLAG_FULL_SNAPSHOT);
  }
}

void test_header_identity_is_constant_across_a_split(void) {
  Captured c;
  uint32_t seq = 0;
  auto rows = makeRows(70);
  hubChunkRows(g_buf, sizeof(g_buf), rows.data(), 70, 0xAABBCCDD, 9, &seq,
               4242, 0, capture, &c);
  for (const auto &p : c.packets) {
    TEST_ASSERT_EQUAL_UINT32(0xAABBCCDD, hdr(p)->device_id);
    TEST_ASSERT_EQUAL_UINT32(9, hdr(p)->boot_id);
    TEST_ASSERT_EQUAL_UINT32(4242, hdr(p)->uptime_ms);
    TEST_ASSERT_EQUAL_UINT8(HUB_PROTO_VERSION, hdr(p)->version);
    TEST_ASSERT_EQUAL_UINT8(HUB_HEADER_LEN, hdr(p)->header_len);
    TEST_ASSERT_EQUAL_UINT8(HUB_RECORD_LEN, hdr(p)->record_len);
    TEST_ASSERT_EQUAL_UINT8('C', hdr(p)->magic[0]);
    TEST_ASSERT_EQUAL_UINT8('D', hdr(p)->magic[1]);
    TEST_ASSERT_EQUAL_UINT8('G', hdr(p)->magic[2]);
    TEST_ASSERT_EQUAL_UINT8('H', hdr(p)->magic[3]);
  }
}

// ---------------------------------------------------------------------------
// Failure and abuse
// ---------------------------------------------------------------------------

void test_transport_failure_stops_rather_than_burning_seq(void) {
  Captured c;
  c.failAfter = 1;                       // accept one packet, refuse the next
  uint32_t seq = 0;
  auto rows = makeRows(200);
  HubChunkResult r = hubChunkRows(g_buf, sizeof(g_buf), rows.data(), 200,
                                  0xAABBCCDD, 3, &seq, 500, 0, capture, &c);
  TEST_ASSERT_TRUE(r.aborted);
  TEST_ASSERT_EQUAL_UINT16(1, r.packets);
  TEST_ASSERT_EQUAL_size_t(1, c.packets.size());
  // Two seq values consumed: the one that went out, and the one on the packet
  // the transport refused. Continuing past a refusal would consume the rest.
  TEST_ASSERT_EQUAL_UINT32(2, seq);
}

void test_undersized_buffer_writes_nothing(void) {
  Captured c;
  uint32_t seq = 0;
  auto rows = makeRows(10);
  uint8_t small[HUB_MAX_PACKET - 1];
  HubChunkResult r = hubChunkRows(small, sizeof(small), rows.data(), 10,
                                  0xAABBCCDD, 3, &seq, 500, 0, capture, &c);
  TEST_ASSERT_EQUAL_UINT16(0, r.packets);
  TEST_ASSERT_EQUAL_size_t(0, c.packets.size());
  TEST_ASSERT_EQUAL_UINT32(0, seq);
}

void test_null_arguments_are_refused(void) {
  Captured c;
  uint32_t seq = 0;
  auto rows = makeRows(4);
  TEST_ASSERT_EQUAL_UINT16(0, hubChunkRows(nullptr, sizeof(g_buf), rows.data(),
                                           4, 1, 1, &seq, 0, 0, capture, &c).packets);
  TEST_ASSERT_EQUAL_UINT16(0, hubChunkRows(g_buf, sizeof(g_buf), nullptr,
                                           4, 1, 1, &seq, 0, 0, capture, &c).packets);
  TEST_ASSERT_EQUAL_UINT16(0, hubChunkRows(g_buf, sizeof(g_buf), rows.data(),
                                           4, 1, 1, nullptr, 0, 0, capture, &c).packets);
  TEST_ASSERT_EQUAL_UINT16(0, hubChunkRows(g_buf, sizeof(g_buf), rows.data(),
                                           4, 1, 1, &seq, 0, 0, nullptr, &c).packets);
}

// ---------------------------------------------------------------------------
// Record content -- the split must not corrupt what it splits.
// ---------------------------------------------------------------------------

void test_record_fields_survive_the_boundary(void) {
  Captured c;
  uint32_t seq = 0;
  auto rows = makeRows(66);
  rows[63].ext = true;                   // last row of packet 1
  rows[64].ext = true;                   // first row of packet 2
  rows[64].dlc = 3;
  hubChunkRows(g_buf, sizeof(g_buf), rows.data(), 66, 1, 1, &seq, 0, 0,
               capture, &c);

  const HubCanRecord *last1 = rec(c.packets[0], 63);
  const HubCanRecord *first2 = rec(c.packets[1], 0);
  TEST_ASSERT_EQUAL_UINT32(0x100u + 63, last1->can_id);
  TEST_ASSERT_TRUE(last1->flags & HUB_REC1_EXTENDED_ID);
  TEST_ASSERT_EQUAL_UINT32(0x100u + 64, first2->can_id);
  TEST_ASSERT_TRUE(first2->flags & HUB_REC1_EXTENDED_ID);
  TEST_ASSERT_EQUAL_UINT8(3, first2->dlc);
  TEST_ASSERT_EQUAL_UINT8(HUB_REC_CAN, first2->rec_type);
  // Protocol 1.3: bytes past dlc must be zero.
  for (uint8_t b = 3; b < 8; b++) TEST_ASSERT_EQUAL_UINT8(0, first2->data[b]);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_zero_rows_emits_nothing_and_burns_no_seq);
  RUN_TEST(test_one_row_is_one_packet);
  RUN_TEST(test_exactly_64_rows_is_still_one_packet);
  RUN_TEST(test_65_rows_splits_into_64_plus_1);
  RUN_TEST(test_129_rows_splits_into_64_64_1);
  RUN_TEST(test_no_packet_ever_exceeds_the_mtu_budget);
  RUN_TEST(test_seq_increments_per_packet_not_per_call);
  RUN_TEST(test_every_row_appears_exactly_once_and_in_order);
  RUN_TEST(test_full_snapshot_flag_rides_on_every_packet_of_a_split);
  RUN_TEST(test_header_identity_is_constant_across_a_split);
  RUN_TEST(test_transport_failure_stops_rather_than_burning_seq);
  RUN_TEST(test_undersized_buffer_writes_nothing);
  RUN_TEST(test_null_arguments_are_refused);
  RUN_TEST(test_record_fields_survive_the_boundary);
  return UNITY_END();
}

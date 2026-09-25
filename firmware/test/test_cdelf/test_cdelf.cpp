// cdElfExtent(): find the ELF inside a stored core dump image, host-tested.
//
// ⚠️ SYNTHETIC images, built below: an IDF-style prefix, a minimal ELF32
// (header, program headers, segment bytes) and a checksum-style trailer. They
// pin the parsing rule. No real dump from this board has been through it yet;
// the first one fetched from GET /api/v1/coredump should be checked with
// `espcoredump.py info_corefile` before this is trusted.

#include <unity.h>

#include <string.h>

#include "cdelf.h"

struct Img {
  uint8_t b[4096];
  uint32_t n;
};

static bool readImg(void *ctx, uint32_t off, void *buf, uint32_t len) {
  const Img *im = (const Img *)ctx;
  if ((uint64_t)off + len > im->n) return false;
  memcpy(buf, im->b + off, len);
  return true;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

// prefix | ELF (52-byte header + nph program headers + segments) | trailer.
// Returns the ELF's true length.
static uint32_t build(Img *im, uint32_t prefix, uint16_t nph,
                      uint32_t segBytes, uint32_t trailer) {
  memset(im, 0, sizeof(*im));
  uint8_t *e = im->b + prefix;
  e[0] = 0x7F; e[1] = 'E'; e[2] = 'L'; e[3] = 'F';
  e[4] = 1;   // ELFCLASS32
  e[5] = 1;   // little-endian
  e[6] = 1;
  put16(e + 16, 4);          // ET_CORE
  put32(e + 28, 52);         // e_phoff
  put32(e + 32, 0);          // e_shoff
  put16(e + 40, 52);         // e_ehsize
  put16(e + 42, 32);         // e_phentsize
  put16(e + 44, nph);        // e_phnum
  put16(e + 46, 40);         // e_shentsize
  put16(e + 48, 0);          // e_shnum
  uint32_t dataOff = 52 + 32u * nph;
  for (uint16_t i = 0; i < nph; i++) {
    uint8_t *ph = e + 52 + 32u * i;
    put32(ph + 0, i == 0 ? 4 : 1);   // PT_NOTE, then PT_LOAD
    put32(ph + 4, dataOff);          // p_offset
    put32(ph + 16, segBytes);        // p_filesz
    dataOff += segBytes;
  }
  for (uint32_t i = 52 + 32u * nph; i < dataOff; i++) e[i] = (uint8_t)i;
  // Trailer bytes that are NOT part of the ELF.
  for (uint32_t i = 0; i < trailer; i++) e[dataOff + i] = 0xEE;
  im->n = prefix + dataOff + trailer;
  return dataOff;
}

static const CdReader *rd(Img *im) {
  static CdReader r;
  r.ctx = im;
  r.read = readImg;
  return &r;
}

static Img g;

void test_finds_the_elf_behind_an_idf_header_and_before_a_checksum(void) {
  const uint32_t len = build(&g, 20, 3, 100, 32);   // 20-byte header, SHA-256 trailer
  uint32_t off = 0, n = 0;
  TEST_ASSERT_TRUE(cdElfExtent(rd(&g), g.n, &off, &n));
  TEST_ASSERT_EQUAL_UINT32(20, off);
  TEST_ASSERT_EQUAL_UINT32(len, n);
  // The trailer is excluded, whatever its size.
  TEST_ASSERT_EQUAL_UINT32(g.n - 20 - 32, n);
}

void test_the_trailer_size_does_not_matter(void) {
  uint32_t off, n;
  const uint32_t a = build(&g, 24, 2, 64, 4);       // CRC32 trailer
  TEST_ASSERT_TRUE(cdElfExtent(rd(&g), g.n, &off, &n));
  TEST_ASSERT_EQUAL_UINT32(a, n);
  const uint32_t b = build(&g, 24, 2, 64, 0);       // no trailer
  TEST_ASSERT_TRUE(cdElfExtent(rd(&g), g.n, &off, &n));
  TEST_ASSERT_EQUAL_UINT32(b, n);
}

void test_elf_at_offset_zero(void) {
  const uint32_t len = build(&g, 0, 1, 16, 8);
  uint32_t off = 99, n = 0;
  TEST_ASSERT_TRUE(cdElfExtent(rd(&g), g.n, &off, &n));
  TEST_ASSERT_EQUAL_UINT32(0, off);
  TEST_ASSERT_EQUAL_UINT32(len, n);
}

void test_no_magic_is_refused(void) {
  build(&g, 20, 2, 32, 4);
  g.b[20] = 0;                                      // break \x7fELF
  uint32_t off, n;
  TEST_ASSERT_FALSE(cdElfExtent(rd(&g), g.n, &off, &n));
}

void test_a_segment_past_the_image_is_refused(void) {
  // A truncated or corrupt dump must not be served as if complete.
  build(&g, 20, 2, 64, 4);
  g.n -= 40;                                        // chop into the last segment
  uint32_t off, n;
  TEST_ASSERT_FALSE(cdElfExtent(rd(&g), g.n, &off, &n));
}

void test_64_bit_elf_and_big_endian_are_refused(void) {
  uint32_t off, n;
  build(&g, 20, 1, 8, 4);
  g.b[20 + 4] = 2;                                  // ELFCLASS64
  TEST_ASSERT_FALSE(cdElfExtent(rd(&g), g.n, &off, &n));
  build(&g, 20, 1, 8, 4);
  g.b[20 + 5] = 2;                                  // big-endian
  TEST_ASSERT_FALSE(cdElfExtent(rd(&g), g.n, &off, &n));
}

void test_hostile_header_fields_do_not_wrap(void) {
  uint32_t off, n;
  build(&g, 20, 1, 8, 4);
  put32(g.b + 20 + 28, 0xFFFFFFF0u);               // e_phoff near 4 GB
  TEST_ASSERT_FALSE(cdElfExtent(rd(&g), g.n, &off, &n));
  build(&g, 20, 1, 8, 4);
  put32(g.b + 20 + 52 + 16, 0xFFFFFFFFu);          // p_filesz 4 GB
  TEST_ASSERT_FALSE(cdElfExtent(rd(&g), g.n, &off, &n));
  build(&g, 20, 1, 8, 4);
  put16(g.b + 20 + 44, 60000);                      // absurd e_phnum
  TEST_ASSERT_FALSE(cdElfExtent(rd(&g), g.n, &off, &n));
}

void test_tiny_image_is_refused(void) {
  memset(&g, 0, sizeof(g));
  g.n = 16;
  uint32_t off, n;
  TEST_ASSERT_FALSE(cdElfExtent(rd(&g), g.n, &off, &n));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_finds_the_elf_behind_an_idf_header_and_before_a_checksum);
  RUN_TEST(test_the_trailer_size_does_not_matter);
  RUN_TEST(test_elf_at_offset_zero);
  RUN_TEST(test_no_magic_is_refused);
  RUN_TEST(test_a_segment_past_the_image_is_refused);
  RUN_TEST(test_64_bit_elf_and_big_endian_are_refused);
  RUN_TEST(test_hostile_header_fields_do_not_wrap);
  RUN_TEST(test_tiny_image_is_refused);
  return UNITY_END();
}

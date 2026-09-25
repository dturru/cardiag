#pragma once

// ---------------------------------------------------------------------------
// Where the ELF is inside a stored core dump image, and how long it is.
//
// The coredump partition does not hold a bare ELF. ESP-IDF writes its own
// header in front and a checksum behind, and both have changed shape across
// IDF versions (the checksum is CRC32 or SHA-256 depending on chip and
// config). Hard-coding either would be a guess about a layout this repo does
// not control.
//
// So this reads the ELF's OWN structure instead: find the \x7fELF magic near
// the start of the image, then take the extent as the furthest byte any
// header, program segment or section table claims. That is exactly the ELF,
// whatever sits around it.
//
// Header-only, stdint + string.h, with the flash read injected, so
// `pio test -e native` covers it (test/test_cdelf) without a build_src_filter
// entry.
// ---------------------------------------------------------------------------

#include <stdint.h>
#include <string.h>

struct CdReader {
  void *ctx;
  // Read `len` bytes at `off` from the START OF THE STORED IMAGE.
  bool (*read)(void *ctx, uint32_t off, void *buf, uint32_t len);
};

// How far into the image to look for the magic. IDF's header is tens of
// bytes; 256 is generous without scanning into the ELF body.
#define CD_ELF_SCAN_BYTES 256u
// Sanity bound on program headers. A core dump has one PT_NOTE plus one
// PT_LOAD per memory region -- dozens, not thousands.
#define CD_ELF_MAX_PHDRS 512u

static inline uint16_t cdU16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t cdU32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

// On success sets *elfOff (offset of the magic within the image) and *elfLen,
// and returns true. Returns false if there is no little-endian ELF32 at a
// 4-byte-aligned offset in the first CD_ELF_SCAN_BYTES, if its headers are
// implausible, or if anything it claims lies past `imageSize`.
static inline bool cdElfExtent(const CdReader *r, uint32_t imageSize,
                               uint32_t *elfOff, uint32_t *elfLen) {
  if (!r || !r->read || imageSize < 52) return false;

  uint8_t head[CD_ELF_SCAN_BYTES];
  const uint32_t scan = imageSize < CD_ELF_SCAN_BYTES ? imageSize : CD_ELF_SCAN_BYTES;
  if (!r->read(r->ctx, 0, head, scan)) return false;

  uint32_t off = 0;
  bool found = false;
  for (uint32_t i = 0; i + 4 <= scan; i += 4) {
    if (head[i] == 0x7F && head[i + 1] == 'E' && head[i + 2] == 'L' &&
        head[i + 3] == 'F') {
      off = i;
      found = true;
      break;
    }
  }
  if (!found || off + 52 > imageSize) return false;

  uint8_t eh[52];
  if (!r->read(r->ctx, off, eh, sizeof(eh))) return false;
  if (eh[4] != 1 /* ELFCLASS32 */ || eh[5] != 1 /* little-endian */) return false;

  const uint32_t phoff = cdU32(eh + 28);
  const uint32_t shoff = cdU32(eh + 32);
  const uint16_t ehsize = cdU16(eh + 40);
  const uint16_t phentsize = cdU16(eh + 42);
  const uint16_t phnum = cdU16(eh + 44);
  const uint16_t shentsize = cdU16(eh + 46);
  const uint16_t shnum = cdU16(eh + 48);

  if (ehsize < 52) return false;
  if (phnum && phentsize != 32) return false;
  if (phnum > CD_ELF_MAX_PHDRS) return false;
  if (shnum && shentsize != 40) return false;

  // 64-bit arithmetic so a hostile offset cannot wrap round to "small".
  uint64_t end = ehsize;
  if (phnum) {
    const uint64_t e = (uint64_t)phoff + (uint64_t)phnum * phentsize;
    if (e > end) end = e;
  }
  if (shnum) {
    const uint64_t e = (uint64_t)shoff + (uint64_t)shnum * shentsize;
    if (e > end) end = e;
  }
  if ((uint64_t)off + end > imageSize) return false;

  for (uint16_t i = 0; i < phnum; i++) {
    uint8_t ph[32];
    if (!r->read(r->ctx, off + phoff + (uint32_t)i * 32u, ph, sizeof(ph)))
      return false;
    const uint64_t e = (uint64_t)cdU32(ph + 4) /* p_offset */ +
                       (uint64_t)cdU32(ph + 16) /* p_filesz */;
    if (e > end) end = e;
  }
  if ((uint64_t)off + end > imageSize) return false;

  *elfOff = off;
  *elfLen = (uint32_t)end;
  return true;
}

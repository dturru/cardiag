#pragma once

// Logger -> hub wire protocol, v1.
//
// This file is the C side of carhub/docs/protocol.md. The Python reference is
// carhub/carhub/proto.py; the two MUST agree byte for byte. If you change a
// field here, change the doc and the Python in the same commit.
//
// LITTLE-ENDIAN EVERYWHERE. The ESP32-S3 is little-endian natively, so nothing
// is byte-swapped -- but that is a property of this target, not a licence to
// assume it. The static_asserts below are the guard rail: the layout is
// naturally aligned today, and a silent padding change is exactly how a wire
// protocol breaks between two languages.

#include <stdint.h>
#include <string.h>

#define HUB_PROTO_VERSION 1
#define HUB_MAGIC0 'C'
#define HUB_MAGIC1 'D'
#define HUB_MAGIC2 'G'
#define HUB_MAGIC3 'H'

#define HUB_HEADER_LEN 32
#define HUB_RECORD_LEN 20
#define HUB_MAX_RECORDS 64
#define HUB_MAX_PACKET (HUB_HEADER_LEN + HUB_MAX_RECORDS * HUB_RECORD_LEN)

// Header flags
#define HUB_FLAG_FULL_SNAPSHOT 0x01
#define HUB_FLAG_FAST          0x02

// Record types
#define HUB_REC_CAN 1
#define HUB_REC_OBD 2

// Record-1 flags
#define HUB_REC1_EXTENDED_ID 0x01
// Record-2 flags
#define HUB_REC2_TRUNCATED   0x01

#pragma pack(push, 1)

struct HubHeader {
  char     magic[4];      // "CDGH"
  uint8_t  version;       // HUB_PROTO_VERSION
  uint8_t  header_len;    // HUB_HEADER_LEN -- receiver skips to records with this
  uint8_t  record_len;    // HUB_RECORD_LEN -- receiver may skip unknown records
  uint8_t  flags;
  uint16_t count;
  uint16_t reserved;      // must be 0
  uint32_t device_id;
  uint32_t boot_id;
  uint32_t seq;
  uint32_t uptime_ms;
  uint32_t reserved2;     // must be 0
};

struct HubCanRecord {
  uint8_t  rec_type;      // HUB_REC_CAN
  uint8_t  dlc;           // 0..8
  uint8_t  changed;       // byte-change mask, same semantics as changes.csv
  uint8_t  flags;         // HUB_REC1_EXTENDED_ID
  uint32_t can_id;
  uint32_t ms;
  uint8_t  data[8];       // bytes past dlc MUST be zero
};

struct HubObdRecord {
  uint8_t  rec_type;      // HUB_REC_OBD
  uint8_t  mode;
  uint16_t pid;           // u16 so Mode 22 two-byte DIDs fit
  uint32_t ms;
  uint8_t  len;           // 0..10
  uint8_t  flags;         // HUB_REC2_TRUNCATED
  uint8_t  payload[10];   // bytes past len MUST be zero
};

#pragma pack(pop)

// The guard rail. If any of these fire, the Python side is already wrong.
static_assert(sizeof(HubHeader) == HUB_HEADER_LEN, "HubHeader must be 32 bytes");
static_assert(sizeof(HubCanRecord) == HUB_RECORD_LEN, "HubCanRecord must be 20 bytes");
static_assert(sizeof(HubObdRecord) == HUB_RECORD_LEN, "HubObdRecord must be 20 bytes");
// Field offsets the hub parses positionally.
static_assert(offsetof(HubHeader, count) == 8, "header.count offset");
static_assert(offsetof(HubHeader, device_id) == 12, "header.device_id offset");
static_assert(offsetof(HubHeader, boot_id) == 16, "header.boot_id offset");
static_assert(offsetof(HubHeader, seq) == 20, "header.seq offset");
static_assert(offsetof(HubHeader, uptime_ms) == 24, "header.uptime_ms offset");
static_assert(offsetof(HubCanRecord, can_id) == 4, "can.can_id offset");
static_assert(offsetof(HubCanRecord, data) == 12, "can.data offset");
static_assert(offsetof(HubObdRecord, ms) == 4, "obd.ms offset");
static_assert(offsetof(HubObdRecord, payload) == 10, "obd.payload offset");

inline void hubHeaderInit(HubHeader *h, uint32_t device_id, uint32_t boot_id,
                          uint32_t seq, uint32_t uptime_ms, uint8_t flags,
                          uint16_t count) {
  h->magic[0] = HUB_MAGIC0; h->magic[1] = HUB_MAGIC1;
  h->magic[2] = HUB_MAGIC2; h->magic[3] = HUB_MAGIC3;
  h->version    = HUB_PROTO_VERSION;
  h->header_len = HUB_HEADER_LEN;
  h->record_len = HUB_RECORD_LEN;
  h->flags      = flags;
  h->count      = count;
  h->reserved   = 0;
  h->device_id  = device_id;
  h->boot_id    = boot_id;
  h->seq        = seq;
  h->uptime_ms  = uptime_ms;
  h->reserved2  = 0;
}

// Fills a CAN record, zero-padding past dlc as the protocol requires.
inline void hubCanRecord(HubCanRecord *r, uint32_t can_id, bool ext,
                         uint8_t dlc, const uint8_t *data, uint8_t changed,
                         uint32_t ms) {
  if (dlc > 8) dlc = 8;
  r->rec_type = HUB_REC_CAN;
  r->dlc      = dlc;
  r->changed  = changed;
  r->flags    = ext ? HUB_REC1_EXTENDED_ID : 0;
  r->can_id   = can_id;
  r->ms       = ms;
  memset(r->data, 0, sizeof(r->data));
  memcpy(r->data, data, dlc);
}

inline void hubObdRecord(HubObdRecord *r, uint8_t mode, uint16_t pid,
                         uint32_t ms, const uint8_t *payload, uint8_t len,
                         bool truncated) {
  if (len > 10) { len = 10; truncated = true; }
  r->rec_type = HUB_REC_OBD;
  r->mode     = mode;
  r->pid      = pid;
  r->ms       = ms;
  r->len      = len;
  r->flags    = truncated ? HUB_REC2_TRUNCATED : 0;
  memset(r->payload, 0, sizeof(r->payload));
  memcpy(r->payload, payload, len);
}

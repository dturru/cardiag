#include "hubchunk.h"

HubChunkResult hubChunkRows(uint8_t *buf, size_t bufCap,
                            const SnifferRow *rows, uint16_t n,
                            uint32_t deviceId, uint32_t bootId,
                            uint32_t *seq, uint32_t uptimeMs,
                            uint8_t flags,
                            HubEmitFn emit, void *ctx) {
  HubChunkResult r = {0, 0, false};
  if (!buf || !rows || !seq || !emit) return r;
  if (bufCap < HUB_MAX_PACKET) return r;

  // Zero rows is a legitimate no-op, not an empty packet: the protocol has no
  // use for a datagram carrying nothing, and sending one would burn a sequence
  // number and make the hub's gap detector report a loss that did not happen.
  if (n == 0) return r;

  uint16_t sent = 0;
  while (sent < n) {
    const uint16_t remaining = (uint16_t)(n - sent);
    const uint16_t chunk =
        remaining > HUB_MAX_RECORDS ? (uint16_t)HUB_MAX_RECORDS : remaining;

    // FULL_SNAPSHOT is a property of the SET, not of a packet, so it rides on
    // every packet of a split snapshot. A hub that saw it on only the first
    // would treat the rest as incremental updates and never learn that the
    // ids in packets 2..N were part of the same full picture.
    HubHeader *h = (HubHeader *)buf;
    hubHeaderInit(h, deviceId, bootId, (*seq)++, uptimeMs, flags, chunk);

    HubCanRecord *rec = (HubCanRecord *)(buf + HUB_HEADER_LEN);
    for (uint16_t i = 0; i < chunk; i++) {
      const SnifferRow &row = rows[sent + i];
      hubCanRecord(&rec[i], row.id, row.ext, row.dlc, row.data,
                   row.changedMask, row.lastMs);
    }

    const size_t len = HUB_HEADER_LEN + (size_t)chunk * HUB_RECORD_LEN;
    if (!emit(buf, len, ctx)) {
      r.aborted = true;
      return r;
    }
    r.packets++;
    r.records = (uint16_t)(r.records + chunk);
    sent = (uint16_t)(sent + chunk);
  }
  return r;
}

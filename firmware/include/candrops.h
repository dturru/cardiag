#pragma once

// ---------------------------------------------------------------------------
// Every place a received CAN frame (or the row made from it) can be lost
// between the transceiver and the file. On /api/v1/session ("can_drops") and
// in the hublink stats line; the soak verdict FAILs on any of them.
//
//   rx_missed          TWAI driver RX queue full (rx_queue_len): canTask did
//                      not drain it in time
//   rx_overrun         TWAI hardware RX FIFO overran before the ISR read it
//   changelog_dropped  recorder change log full: canTask -> filestore ring
//                      (recorder.h), rows refused rather than wrapped
//   id_overflow        sniffer table full: frames of ids past SNIFF_MAX_IDS
//                      never reach the Tier B snapshot
//   raw_ring_busy      raw ring's lock was held by a reader; canTask only
//                      TRIES it and counts the frame instead of waiting
//
// The two TWAI counters are the driver's, and restart whenever the driver is
// reinstalled (a mode change). The other two are since boot or since the
// recorder/sniffer was last cleared. All four only ever grow between those
// points, so "non-zero" is the whole test.
// ---------------------------------------------------------------------------

#include <stdint.h>

struct CanDrops {
  bool     twaiUp;
  uint32_t rxMissed;
  uint32_t rxOverrun;
  uint32_t changelogDropped;
  uint32_t idOverflow;
  uint32_t rawRingBusy;
  const char *changeLog;   // recorderChangeLogStatus(): "ok" or why it is off
};

CanDrops cardiagCanDrops();

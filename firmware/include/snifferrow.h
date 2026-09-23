#pragma once

// One row of the per-ID sniffer table.
//
// This lives in its own header, apart from sniffer.h, for one reason: sniffer.h
// pulls in driver/twai.h (it takes a twai_message_t), which makes anything that
// includes it ESP32-only. SnifferRow itself is a plain POD with no target
// dependency, so splitting it out is what lets the packet serialiser be
// compiled and tested ON THE HOST -- see hubchunk.h and test/test_hubchunk/.
//
// Nothing else changes: sniffer.h includes this file, so every existing
// #include "sniffer.h" still sees SnifferRow exactly as before.

#include <stdint.h>

struct SnifferRow {
  uint32_t id;
  uint32_t lastMs;       // when this id was last seen
  uint32_t changedAtMs;  // when its payload last DIFFERED from the previous one
  uint8_t  data[8];
  uint8_t  dlc;
  uint8_t  changedMask;
  bool     ext;
};

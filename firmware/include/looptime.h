#pragma once

// loop() latency since boot. Measured in main.cpp's loop(); read by the
// session API and the hublink stats line (which the soak CSV parses).
//
// Why it exists: the Wi-Fi join became a step-per-pass state machine
// (hublink.cpp) on the promise that no call in loop() blocks more than
// ~100 ms. This is the number that says whether that holds on a real board.

#include <stdint.h>

struct LoopStats {
  uint32_t    maxUs;        // longest loop() pass since boot, microseconds
  uint32_t    maxStageUs;   // the slowest stage within that pass
  const char *maxStage;     // its name: "hublink", "webui", "filestore", ...
  uint32_t    passes;       // passes since boot
  uint32_t    over100ms;    // passes longer than 100 ms
};

const LoopStats *cardiagLoopStats();

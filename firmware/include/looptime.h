#pragma once

// loop() latency since boot. Measured in main.cpp's loop(); read by the
// session API and the hublink stats line (which the soak CSV parses).
//
// Why it exists: the Wi-Fi join became a step-per-pass state machine
// (hublink.cpp) on the promise that no call in loop() blocks more than
// ~100 ms. This is the number that says whether that holds on a real board.

#include <stdint.h>

struct LoopStats {
  uint32_t    maxUs;        // longest loop() pass in the window, microseconds
  uint32_t    maxStageUs;   // the slowest stage within that pass
  const char *maxStage;     // its name: "hublink", "webui", "filestore", ...
  uint32_t    passes;       // passes in the window
  uint32_t    over100ms;    // passes longer than 100 ms
};

// Since boot. Never reset: the worst pass this boot has ever had.
const LoopStats *cardiagLoopStats();

// ⭐ PER-INTERVAL windows. A since-boot max says a 3.7 s pass happened once;
// it cannot say whether it is still happening. Each window below holds the
// same numbers since it was last TAKEN, and taking it resets it.
//
//   api   taken by each GET /api/v1/session ("loop.interval")
//   log   taken by each hublink stats line  ("loopwin=" / "loopwinstage=")
//
// Separate so a dashboard polling the session cannot empty the window the
// soak reads from serial, and vice versa. Both are touched only on the loop()
// task (the web server and the stats line both run inside loop()), so no lock.
enum LoopWindowId { LOOP_WIN_API = 0, LOOP_WIN_LOG = 1 };
LoopStats cardiagLoopTakeWindow(LoopWindowId w);

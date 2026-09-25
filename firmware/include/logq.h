#pragma once

// ---------------------------------------------------------------------------
// ONE WRITER FOR SERIAL: the loop() task.
//
// WHY (2026-09-25 baseline soak). canTask printed every frame in LISTEN and
// SELFTEST with ~10 separate Serial writes per line, from its own task, while
// loop() printed the ~335-char hublink stats line. The two interleaved
// mid-line, and 16 of 40 soak cycles had fs_sub_* and drop counters that
// could not be parsed. A blocked USB CDC write could also stall canTask.
//
// Now anything that is NOT on the loop() task hands its line to this queue
// and returns at once -- a full queue drops the line and counts it, it never
// waits -- and loop() writes queued lines out whole, between its own prints.
// ESP-IDF's own logging (esp_log, the Wi-Fi driver) is routed here too.
// Every byte on Serial therefore comes from one task, a whole line at a time.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

// Call once in setup(), before any other task starts. Also redirects esp_log.
void logqBegin();

// From any task, including canTask. Never blocks. `line` should end in '\n'.
void logqPush(const char *line, size_t len);
void logqPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// loop() only: write out queued lines, whole, up to `budgetBytes`.
void logqDrain(size_t budgetBytes);

// Lines dropped because the queue was full (a stalled loop(), or a frame
// print rate the port cannot carry). Printed on the [stats] line.
uint32_t logqDropped();

#pragma once

// Runtime half of the boot guard: where the counter is stored (RTC_NOINIT
// memory + NVS) and the early watchdog. The decision logic is in bootguard.h
// and is host-tested; this file is the plumbing and is not.

#include <stdint.h>

// Reads both copies of the counter and returns the number of filestore starts
// that began and never finished, for THIS firmware build. Prints one line.
uint8_t bootguardBegin();

// Records that a filestore start is beginning (prior + 1, both copies). Call
// immediately before filestoreBegin(), after the early watchdog is armed.
void bootguardMarkStart(uint8_t prior);

// The start finished. Writes 0 to both copies.
void bootguardClear();

// Unfinished starts before this boot, as read by bootguardBegin().
uint8_t bootguardPriorAttempts();

// (Re)arms the task watchdog for the calling task with this timeout, and
// feeds it once so the new window starts now.
void bootguardArmTaskWdt(uint32_t seconds);

#pragma once

// ---------------------------------------------------------------------------
// The stored core dump: KEPT UNTIL THE HUB HAS IT.
//
// 🐛 WHY (2026-09-25). The 200-cycle soak had one TASK_WDT reset, at cycle 135,
// and its coredump was lost: setup() printed a six-frame summary and then
// erased the dump on the same boot. A summary line says a crash happened and
// roughly where; the ELF says which task, which PC, what the stack held. The
// one crash worth diagnosing was reduced to a line of serial that the soak
// harness had to be running to catch.
//
// Now the dump is only ever erased by an acknowledgement, the same pattern as
// the file watermark:
//
//   GET  /api/v1/coredump        the ELF (or ?format=raw: the whole stored
//                                image), X-Hub-Token, with X-Coredump-Sha256
//   POST /api/v1/coredump/ack    {"sha256":"<hex>"} -- erased ONLY if that
//                                matches the dump stored now, so an ack for an
//                                old dump can never erase a newer one
//   /api/v1/session              "coredump":{"present":..,"bytes":..,"sha256":..}
//
// The boot summary still prints, every boot a dump is present, marked as
// possibly older than this boot.
// ---------------------------------------------------------------------------

#include <stdint.h>

class WebServer;

// Call once in setup(), after the reset reason is known. Finds the stored
// image, locates the ELF in it, hashes it, prints the summary. NEVER erases.
void coredumpBegin(int resetReason);

bool coredumpPresent();
// Bytes the default GET serves (the ELF, or the raw image if no ELF parses).
uint32_t coredumpBytes();
// Lowercase hex sha256 of those bytes; "" when absent.
const char *coredumpSha256Hex();
// "elf" or "raw": what the default GET serves.
const char *coredumpFormat();

// ⭐ LIFETIME CRASH COUNT, NVS. ESP-IDF keeps ONE dump and overwrites it on
// every crash ("latest wins"), so a board that crashes twice before the hub
// fetches loses the first dump. This counter is incremented at boot whenever
// the reset reason is PANIC or a watchdog, so an overwritten crash is at
// least COUNTED: crashes_total - dumps_acked - (present ? 1 : 0) crashes left
// no dump the hub ever saw.
uint32_t coredumpCrashesTotal();
uint32_t coredumpDumpsAcked();
// Name of the last crash's reset reason ("TASK_WDT", ...), "" if none ever.
const char *coredumpLastCrashReason();

void coredumpRegister(WebServer &srv);

#pragma once

// Wi-Fi access point + web UI.
//
// There is no network to join in a car, so the board hosts its own AP. A phone
// connects to it and opens http://192.168.4.1/ to get the same sniffer table
// that the serial console prints -- which is the whole point: no laptop.
//
// The page POLLS a small JSON snapshot a few times a second. It does not stream
// frames. Pushing 1000 fps over Wi-Fi is precisely how a sniffer turns into a
// frame-dropper, and the aggregated table is the thing worth looking at anyway.
//
// SAFETY: the web UI exposes ONLY the passive modes. Nothing reachable over the
// air can put a frame on the vehicle bus. That matches the rule the BOOT button
// follows -- transmitting always takes a deliberate, confirmed keystroke on the
// wire.

#include <stdint.h>

void webuiStart();     // brings up the AP and the server
void webuiStop();      // tears both down and powers the radio off
void webuiLoop();      // call from loop(); cheap no-op while stopped
bool webuiRunning();

// Implemented in main.cpp. Declared here so the web layer can read state and
// request a mode change without owning either.
uint32_t    cardiagFrames();
uint32_t    cardiagMissed();
uint32_t    cardiagBusErr();
const char *cardiagModeName();

// Returns false if the mode is not one the UI is allowed to select.
bool cardiagSetPassiveMode(uint8_t m);

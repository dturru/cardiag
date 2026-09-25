#pragma once

// /api/v1/* -- the hub-facing HTTP surface. Protocol v1 section 2.
//
// Additive: the existing routes (/, /api/table, /api/mode, /api/clear,
// /api/rec, /api/raw.csv, /api/changes.csv) are untouched, so the standalone
// UI works exactly as before with no hub present.
//
// Phase A implements /session and /time only. The file endpoints need a file
// store, which needs either LittleFS or the carrier board's microSD.

#include <WebServer.h>

void hubapiRegister(WebServer &srv);

// The shared X-Hub-Token check (constant-time), for other modules' endpoints.
bool hubapiTokenOk(WebServer &srv);

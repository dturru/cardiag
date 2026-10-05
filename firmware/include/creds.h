#pragma once

// Logger -> hub link credentials, stored in NVS (namespace "cardiaglink").
//
// Provisioned once over serial (`:cred ...`, see credstore.h). NVS lives at
// 0x9000 and an app upload does not touch it, so a reflash keeps them; only a
// full flash erase clears them. Nothing is compiled in: with nothing
// provisioned the logger runs STANDALONE -- its own AP, no hub join -- and
// says so at boot.
//
// Values never leave this module: a secret shows as "set, N chars", other
// fields as a fingerprint (credDescribe, credstore.h).

#include <stddef.h>
#include <stdint.h>

// Load the active set from NVS and print the boot banner. Call once, early in
// setup(), before hublinkBegin(). The ONLY NVS read outside a console command;
// the active set is fixed until the next boot. `cred set` / `clear` only
// stage; `cred commit` validates the whole set and writes it under a
// `complete` flag, so an interrupted commit boots as NOT provisioned.
void credsBegin();

// Hub Wi-Fi (the hub's AP) provisioned: SSID and passphrase both present.
bool        credsHaveHubWifi();
const char *credsHubSsid();   // "" if not provisioned
const char *credsHubPass();   // "" if not provisioned

// Hub IPv4 for the live UDP stream, if `cred set hub_addr` was committed.
// False = unset: hublink sends to the Wi-Fi gateway (right on the hub's own
// AP; wrong on a laptop hotspot, where the gateway is the laptop).
bool credsHubAddr(uint8_t out[4]);

// Hub mDNS hostname (without ".local"): `cred set hub_name`, else "carhub".
const char *credsHubName();

// This logger's mDNS hostname (without ".local"): `cred set logger_name`,
// else "cardiag".
const char *credsLoggerName();

// X-Hub-Token check, constant time. False when no token is provisioned: the
// mutating endpoints are LOCKED until one is, never open.
bool credsTokenOk(const char *got, size_t n);

// Serial console hook. Returns true if it consumed `ch`: a line starting with
// ':' (and every line of a `:cred ca` PEM) belongs to this module and never
// reaches the single-key handler. Characters are not echoed.
bool credsConsoleFeed(int ch);
// Call from loop(): drops a half-typed line after CREDS_LINE_IDLE_MS.
void credsConsoleTick();

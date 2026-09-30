#pragma once

// Logger -> hub link credentials: the PURE half (no Arduino, no NVS).
//
// Validation, the `cred ...` console-line parser, the SHA-256 fingerprint
// printed in place of a value, PEM -> DER for the CA, and the constant-time
// token compare. creds.cpp is the NVS + serial glue around it; this half is
// what test/test_credstore exercises on the host.
//
// NOTHING HERE PRINTS. A value only ever leaves this module as a fingerprint.

#include <stddef.h>
#include <stdint.h>

enum CredField : uint8_t {
  CRED_SSID = 0,     // the hub's AP, which the logger joins as a station
  CRED_PASS,         // its WPA2 passphrase (8-63) or hex PSK (64)
  CRED_MQTT_USER,
  CRED_MQTT_PASS,
  CRED_TOKEN,        // X-Hub-Token for the mutating /api/v1 endpoints
  CRED_CA,           // hub CA certificate, PEM, exactly one cert
  CRED_FIELD_COUNT,
  CRED_NONE = 0xFF,
};

#define CRED_SSID_MAX       32
#define CRED_PASS_MIN       8
#define CRED_PASS_MAX       64
#define CRED_MQTT_USER_MAX  64
#define CRED_MQTT_PASS_MAX  128
#define CRED_TOKEN_MIN      8
#define CRED_TOKEN_MAX      128
#define CRED_CA_MAX         4096
// One console line. A PEM body line is 64; `cred set mqtt_pass <128>` is the
// longest command.
#define CRED_LINE_MAX       160

// Console / NVS names. NVS keys are <= 15 chars.
const char *credFieldName(CredField f);
const char *credNvsKey(CredField f);
CredField   credFieldFromName(const char *s, size_t n);
size_t      credFieldMax(CredField f);

enum CredErr : uint8_t {
  CRED_OK = 0,
  CRED_E_EMPTY,
  CRED_E_SHORT,
  CRED_E_LONG,
  CRED_E_CHARSET,
  CRED_E_PLACEHOLDER,   // a value from secrets.h.example / the old defaults
  CRED_E_PEM,
};
const char *credErrName(CredErr e);

// Placeholder values shipped in secrets.h.example or as config.h fallbacks.
// Secret fields reject them outright; import skips them.
bool    credIsPlaceholder(const char *v, size_t n);
CredErr credValidate(CredField f, const char *v, size_t n);

// ---------------------------------------------------------------------------
// Console line (without the leading ':' that puts the console in line mode):
//
//   cred show
//   cred set <ssid|pass|mqtt_user|mqtt_pass|token> <value...>
//   cred ca                     then the PEM, ending at -----END CERTIFICATE-----
//   cred clear <field|all>
//   cred import                 one-time, from a legacy secrets.h
//   cred help
//
// A value is everything after the single space that follows the field name,
// so an SSID may contain spaces. Trailing CR/LF is not part of it.
// ---------------------------------------------------------------------------
enum CredCmd : uint8_t {
  CRED_CMD_NONE = 0,   // not a `cred` line
  CRED_CMD_BAD,        // a `cred` line that does not parse
  CRED_CMD_HELP,
  CRED_CMD_SHOW,
  CRED_CMD_SET,
  CRED_CMD_CA,
  CRED_CMD_CLEAR,
  CRED_CMD_IMPORT,
};

struct CredParsed {
  CredCmd     cmd;
  CredField   field;     // SET / CLEAR; CRED_NONE for `clear all`
  bool        all;       // CLEAR all
  const char *value;     // SET: points into the caller's line
  size_t      valueLen;
};

CredParsed credParseLine(const char *line, size_t n);

// ---------------------------------------------------------------------------
// Fingerprints.
//
// "sha256:" + the first 8 hex of SHA-256(value): enough to tell "same value as
// on my laptop" from "not", and all that is ever printed. Check one locally:
//     printf %s 'value' | sha256sum | cut -c1-8
// For the CA the hash is over the DER, so it matches the first 8 hex of
//     openssl x509 -in ca.pem -noout -fingerprint -sha256
// ---------------------------------------------------------------------------
#define CRED_FP_LEN 15   // "sha256:" + 8 hex
void credSha256(const uint8_t *d, size_t n, uint8_t out[32]);
void credFingerprint(const uint8_t *d, size_t n, char out[CRED_FP_LEN + 1]);

// Exactly one BEGIN/END CERTIFICATE block with a valid base64 body. Writes the
// DER to `der` (cap bytes). False on anything malformed or oversized.
bool credPemToDer(const char *pem, size_t n, uint8_t *der, size_t cap,
                  size_t *derLen);
// CA fingerprint as above; "sha256:????????" if the PEM does not decode.
void credCaFingerprint(const char *pem, size_t n, char out[CRED_FP_LEN + 1]);

// Constant-time in the length of `want`. An EMPTY `want` never matches: an
// unprovisioned token must lock the endpoints, not open them.
bool credTokenEqual(const char *want, size_t wlen, const char *got, size_t glen);

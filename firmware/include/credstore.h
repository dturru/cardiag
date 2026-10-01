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
// PEM bytes. The hub CA is EC P-256 (~0.7 kB PEM); 2 kB also fits RSA-2048/3072.
// Kept well under the NVS budget: the whole nvs partition is 20 kB (0x5000),
// shared with bootguard and the mode prefs, and a commit that replaces the CA
// briefly holds old and new copies.
#define CRED_CA_MAX         2048
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
  CRED_E_PAIR,          // ssid without pass, mqtt_user without mqtt_pass...
  CRED_E_NOMEM,         // no heap for the CA scratch (it is not kept resident)
};
const char *credErrName(CredErr e);

// Placeholder values shipped in secrets.h.example or as config.h fallbacks.
// Secret fields reject them outright; import skips them.
bool    credIsPlaceholder(const char *v, size_t n);
CredErr credValidate(CredField f, const char *v, size_t n);

// Fields whose value is a secret: pass, mqtt_pass, token.
bool credIsSecret(CredField f);

// The WHOLE set a commit would leave behind, by presence. ssid/pass and
// mqtt_user/mqtt_pass go together or not at all. On failure *bad names the
// field whose partner is missing.
CredErr credCheckSet(const bool present[CRED_FIELD_COUNT], CredField *bad);

// What the store's commit flag says, from three NVS facts: does the `complete`
// key exist, its value, and is any field key present. `cred commit` writes
// complete=0 FIRST, so the key existing is what separates a board that was
// never provisioned (no key, no fields) from an interrupted commit (key = 0,
// or fields with no key at all).
enum CredCommitState : uint8_t {
  CRED_COMMIT_NONE = 0,     // never provisioned
  CRED_COMMIT_INCOMPLETE,   // a commit started and did not finish
  CRED_COMMIT_COMPLETE,
};
CredCommitState credCommitState(bool flagPresent, uint8_t flag, bool anyField);
// "not provisioned" / "INCOMPLETE" / "complete" -- the boot banner's word.
const char *credCommitStateName(CredCommitState s);

// ---------------------------------------------------------------------------
// Console line (without the leading ':' that puts the console in line mode):
//
//   cred show
//   cred set <ssid|pass|mqtt_user|mqtt_pass|token> <value...>   (staged)
//   cred ca                     then the PEM, ending at -----END CERTIFICATE-----
//   cred clear <field|all>                                       (staged)
//   cred commit                 validate the whole set, then write it
//   cred abort                  drop everything staged
//   cred import                 one-time, from a legacy secrets.h
//   cred help
//
// A value is LITERAL: every byte after the single space that follows the field
// name, up to the line end. Spaces (leading, inner, trailing) and quotes are
// part of it; there is no quoting or escaping. Trailing CR/LF is not part of
// it.
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
  CRED_CMD_COMMIT,
  CRED_CMD_ABORT,
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
// What is printed in place of a value (credDescribe).
//
//   secrets (pass, mqtt_pass, token)  "set, N chars" -- NO hash. A plain hash
//       of a low-entropy passphrase is an offline guess-checker for anyone
//       holding a serial log; a keyed hash could not be checked on a laptop,
//       which was its only use. The length is enough to catch a truncated
//       or whitespace-padded paste.
//   ssid, mqtt_user                   "sha256:" + first 8 hex of SHA-256.
//       Not secrets (the SSID is broadcast), just kept out of pasted logs.
//       Check locally:  printf %s 'value' | sha256sum | cut -c1-8
//   ca                                SHA-256 of the DER: the first 8 hex of
//       openssl x509 -in ca.pem -noout -fingerprint -sha256
//       A CA is public; this is the standard way to name one.
// ---------------------------------------------------------------------------
#define CRED_DESC_LEN 24
void credDescribe(CredField f, const char *v, size_t n, char out[CRED_DESC_LEN]);

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

// credstore.h: validation, the `cred` line parser, fingerprints, PEM -> DER,
// and the constant-time token compare.

#include <string.h>
#include <unity.h>

#include "credstore.h"

void setUp(void) {}
void tearDown(void) {}

static CredErr v(CredField f, const char *s) { return credValidate(f, s, strlen(s)); }
static CredParsed parse(const char *s) { return credParseLine(s, strlen(s)); }
static bool tokEq(const char *w, const char *g) {
  return credTokenEqual(w, strlen(w), g, strlen(g));
}

// A throwaway self-signed test CA (no key exists anywhere). Its
// `openssl x509 -fingerprint -sha256` starts 4C:01:66:BE.
static const char kPem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBiTCCAS+gAwIBAgIUBVIFm1w6fmZ9zDpK65e7wD2Hg+kwCgYIKoZIzj0EAwIw\n"
    "GjEYMBYGA1UEAwwPY2FyZGlhZy10ZXN0LWNhMB4XDTI2MDkzMDE4MjUyMVoXDTM2\n"
    "MDkyNzE4MjUyMVowGjEYMBYGA1UEAwwPY2FyZGlhZy10ZXN0LWNhMFkwEwYHKoZI\n"
    "zj0CAQYIKoZIzj0DAQcDQgAETFJDkiXeHQ+5MexrT1wYttK5O3v2F2wMkBZNXwgr\n"
    "uAPsSnLykBjySfwwnnasMWP/lmfVfJgvmseLo5XCPZR8uqNTMFEwHQYDVR0OBBYE\n"
    "FDDWfPcH2+0NN2ROFS1uLjnWe1WMMB8GA1UdIwQYMBaAFDDWfPcH2+0NN2ROFS1u\n"
    "LjnWe1WMMA8GA1UdEwEB/wQFMAMBAf8wCgYIKoZIzj0EAwIDSAAwRQIhALGM6+Dd\n"
    "THVQzZF6KplqRYykju6J4+23dMVXvVMs4hWZAiA+MoJ46dlE3ywx794kwYWxJO7h\n"
    "eUm6v2LBPI2MrfyrtQ==\n"
    "-----END CERTIFICATE-----\n";

// --- SHA-256 / fingerprint --------------------------------------------------

void test_sha256_known_vectors(void) {
  uint8_t d[32];
  credSha256((const uint8_t *)"abc", 3, d);
  TEST_ASSERT_EQUAL_HEX8(0xba, d[0]);
  TEST_ASSERT_EQUAL_HEX8(0xad, d[31]);          // ...f20015ad
  credSha256((const uint8_t *)"", 0, d);
  TEST_ASSERT_EQUAL_HEX8(0xe3, d[0]);           // e3b0c442...
  TEST_ASSERT_EQUAL_HEX8(0x55, d[31]);          // ...7852b855
  // 56 bytes: the padding spills into a second block.
  const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  credSha256((const uint8_t *)m, strlen(m), d);
  TEST_ASSERT_EQUAL_HEX8(0x24, d[0]);           // 248d6a61...
  TEST_ASSERT_EQUAL_HEX8(0xc1, d[31]);          // ...19db06c1
}

void test_fingerprint_matches_sha256sum_prefix(void) {
  char fp[CRED_FP_LEN + 1];
  // printf %s 'hunter2-hunter2' | sha256sum | cut -c1-8
  credFingerprint((const uint8_t *)"hunter2-hunter2", 15, fp);
  TEST_ASSERT_EQUAL_STRING("sha256:84ca4458", fp);
  TEST_ASSERT_EQUAL(CRED_FP_LEN, strlen(fp));
}

void test_fingerprint_never_contains_the_value(void) {
  char fp[CRED_FP_LEN + 1];
  credFingerprint((const uint8_t *)"abc", 3, fp);
  TEST_ASSERT_EQUAL_STRING("sha256:ba7816bf", fp);
  TEST_ASSERT_NULL(strstr(fp, "abc"));
}

// --- PEM -----------------------------------------------------------------------

void test_ca_fingerprint_matches_openssl(void) {
  char fp[CRED_FP_LEN + 1];
  credCaFingerprint(kPem, strlen(kPem), fp);
  TEST_ASSERT_EQUAL_STRING("sha256:4c0166be", fp);
  TEST_ASSERT_EQUAL(CRED_OK, v(CRED_CA, kPem));
}

void test_pem_crlf_and_no_trailing_newline_ok(void) {
  char buf[sizeof(kPem) * 2];
  size_t o = 0;
  for (const char *p = kPem; *p; p++) {
    if (*p == '\n') buf[o++] = '\r';
    buf[o++] = *p;
  }
  buf[o] = 0;
  char fp[CRED_FP_LEN + 1];
  credCaFingerprint(buf, o - 2, fp);            // drop the final CRLF
  TEST_ASSERT_EQUAL_STRING("sha256:4c0166be", fp);
}

void test_pem_rejects_malformed(void) {
  TEST_ASSERT_EQUAL(CRED_E_PEM, v(CRED_CA, "not a cert"));
  TEST_ASSERT_EQUAL(CRED_E_PEM, v(CRED_CA,
      "-----BEGIN CERTIFICATE-----\nMIIB\n"));                    // no END
  TEST_ASSERT_EQUAL(CRED_E_PEM, v(CRED_CA,
      "-----BEGIN CERTIFICATE-----\nMII*\n-----END CERTIFICATE-----\n"));
  TEST_ASSERT_EQUAL(CRED_E_PEM, v(CRED_CA,
      "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n"));  // not a SEQUENCE
  TEST_ASSERT_EQUAL(CRED_E_PEM, v(CRED_CA,
      "-----BEGIN CERTIFICATE-----\n\n-----END CERTIFICATE-----\n"));      // empty
  // A private key pasted by mistake is not a certificate.
  TEST_ASSERT_EQUAL(CRED_E_PEM, v(CRED_CA,
      "-----BEGIN PRIVATE KEY-----\nMIIB\n-----END PRIVATE KEY-----\n"));
}

void test_pem_rejects_two_certs_and_junk_outside(void) {
  static char two[sizeof(kPem) * 2];
  strcpy(two, kPem);
  strcat(two, kPem);
  TEST_ASSERT_EQUAL(CRED_E_PEM, v(CRED_CA, two));
  static char junk[sizeof(kPem) + 8];
  strcpy(junk, "x\n");
  strcat(junk, kPem);
  TEST_ASSERT_EQUAL(CRED_E_PEM, v(CRED_CA, junk));
}

// --- Validation ----------------------------------------------------------------

void test_ssid_limits(void) {
  TEST_ASSERT_EQUAL(CRED_E_EMPTY, v(CRED_SSID, ""));
  TEST_ASSERT_EQUAL(CRED_OK, v(CRED_SSID, "a"));
  TEST_ASSERT_EQUAL(CRED_OK, v(CRED_SSID, "with spaces ok"));
  TEST_ASSERT_EQUAL(CRED_OK, v(CRED_SSID, "0123456789abcdef0123456789abcdef"));   // 32
  TEST_ASSERT_EQUAL(CRED_E_LONG, v(CRED_SSID, "0123456789abcdef0123456789abcdef0"));
  TEST_ASSERT_EQUAL(CRED_E_CHARSET, v(CRED_SSID, "tab\there"));
}

void test_wpa_pass_limits(void) {
  TEST_ASSERT_EQUAL(CRED_E_SHORT, v(CRED_PASS, "1234567"));
  TEST_ASSERT_EQUAL(CRED_OK, v(CRED_PASS, "12345678"));
  TEST_ASSERT_EQUAL(CRED_OK, v(CRED_PASS, "spaces are fine here"));
  char p63[64]; memset(p63, 'x', 63); p63[63] = 0;
  TEST_ASSERT_EQUAL(CRED_OK, v(CRED_PASS, p63));
  char p64[65]; memset(p64, 'a', 64); p64[64] = 0;            // hex PSK
  TEST_ASSERT_EQUAL(CRED_OK, v(CRED_PASS, p64));
  p64[10] = 'g';                                               // not hex
  TEST_ASSERT_EQUAL(CRED_E_CHARSET, v(CRED_PASS, p64));
  char p65[66]; memset(p65, 'a', 65); p65[65] = 0;
  TEST_ASSERT_EQUAL(CRED_E_LONG, v(CRED_PASS, p65));
  TEST_ASSERT_EQUAL(CRED_E_CHARSET, v(CRED_PASS, "caf\xc3\xa9-latte"));
}

void test_placeholders_rejected_for_secrets(void) {
  TEST_ASSERT_EQUAL(CRED_E_PLACEHOLDER, v(CRED_PASS, "carhub-default"));
  TEST_ASSERT_EQUAL(CRED_E_PLACEHOLDER, v(CRED_PASS, "changeme123"));
  TEST_ASSERT_EQUAL(CRED_E_PLACEHOLDER, v(CRED_TOKEN, "change-me"));
  TEST_ASSERT_EQUAL(CRED_E_PLACEHOLDER, v(CRED_MQTT_PASS, "cardiag-default"));
  TEST_ASSERT_TRUE(credIsPlaceholder("change-me", 9));
  TEST_ASSERT_FALSE(credIsPlaceholder("change-me2", 10));
}

void test_token_and_mqtt(void) {
  TEST_ASSERT_EQUAL(CRED_E_SHORT, v(CRED_TOKEN, "short"));
  TEST_ASSERT_EQUAL(CRED_OK, v(CRED_TOKEN, "a-long-enough-token"));
  TEST_ASSERT_EQUAL(CRED_E_CHARSET, v(CRED_TOKEN, "has a space in it"));
  TEST_ASSERT_EQUAL(CRED_OK, v(CRED_MQTT_USER, "logger"));
  TEST_ASSERT_EQUAL(CRED_E_CHARSET, v(CRED_MQTT_USER, "two words"));
  TEST_ASSERT_EQUAL(CRED_OK, v(CRED_MQTT_PASS, "any printable, spaces ok"));
  TEST_ASSERT_EQUAL(CRED_E_CHARSET, v(CRED_MQTT_PASS, "bell\x07"));
}

// --- Parser --------------------------------------------------------------------

void test_parse_set_value_keeps_inner_spaces_and_drops_crlf(void) {
  const CredParsed p = parse("cred set ssid My Hub AP\r\n");
  TEST_ASSERT_EQUAL(CRED_CMD_SET, p.cmd);
  TEST_ASSERT_EQUAL(CRED_SSID, p.field);
  TEST_ASSERT_EQUAL(9, p.valueLen);
  TEST_ASSERT_EQUAL_MEMORY("My Hub AP", p.value, 9);
}

void test_parse_set_every_field(void) {
  const char *names[] = {"ssid", "pass", "mqtt_user", "mqtt_pass", "token"};
  for (uint8_t i = 0; i < 5; i++) {
    char line[64];
    strcpy(line, "cred set ");
    strcat(line, names[i]);
    strcat(line, " x");
    const CredParsed p = parse(line);
    TEST_ASSERT_EQUAL(CRED_CMD_SET, p.cmd);
    TEST_ASSERT_EQUAL(i, p.field);
    TEST_ASSERT_EQUAL(1, p.valueLen);
  }
}

void test_parse_set_empty_value_is_a_set_that_validation_rejects(void) {
  const CredParsed p = parse("cred set pass");
  TEST_ASSERT_EQUAL(CRED_CMD_SET, p.cmd);
  TEST_ASSERT_EQUAL(0, p.valueLen);
}

void test_parse_other_commands(void) {
  TEST_ASSERT_EQUAL(CRED_CMD_SHOW, parse("cred show").cmd);
  TEST_ASSERT_EQUAL(CRED_CMD_HELP, parse("cred").cmd);
  TEST_ASSERT_EQUAL(CRED_CMD_HELP, parse("cred help").cmd);
  TEST_ASSERT_EQUAL(CRED_CMD_CA, parse("cred ca\r").cmd);
  TEST_ASSERT_EQUAL(CRED_CMD_IMPORT, parse("cred import").cmd);
  CredParsed p = parse("cred clear all");
  TEST_ASSERT_EQUAL(CRED_CMD_CLEAR, p.cmd);
  TEST_ASSERT_TRUE(p.all);
  p = parse("cred clear ca");
  TEST_ASSERT_EQUAL(CRED_CMD_CLEAR, p.cmd);
  TEST_ASSERT_FALSE(p.all);
  TEST_ASSERT_EQUAL(CRED_CA, p.field);
}

void test_parse_rejects(void) {
  TEST_ASSERT_EQUAL(CRED_CMD_NONE, parse("").cmd);
  TEST_ASSERT_EQUAL(CRED_CMD_NONE, parse("creds show").cmd);
  TEST_ASSERT_EQUAL(CRED_CMD_BAD, parse("cred set").cmd);
  TEST_ASSERT_EQUAL(CRED_CMD_BAD, parse("cred set bogus x").cmd);
  TEST_ASSERT_EQUAL(CRED_CMD_BAD, parse("cred set ca x").cmd);   // multi-line only
  TEST_ASSERT_EQUAL(CRED_CMD_BAD, parse("cred clear").cmd);
  TEST_ASSERT_EQUAL(CRED_CMD_BAD, parse("cred clear bogus").cmd);
  TEST_ASSERT_EQUAL(CRED_CMD_BAD, parse("cred clear all now").cmd);
  TEST_ASSERT_EQUAL(CRED_CMD_BAD, parse("cred show pass").cmd);
  TEST_ASSERT_EQUAL(CRED_CMD_BAD, parse("cred frob").cmd);
}

// --- Token compare ---------------------------------------------------------------

void test_token_equal(void) {
  TEST_ASSERT_TRUE(tokEq("a-long-enough-token", "a-long-enough-token"));
  TEST_ASSERT_FALSE(tokEq("a-long-enough-token", "a-long-enough-tokeN"));
  TEST_ASSERT_FALSE(tokEq("a-long-enough-token", "a-long-enough-toke"));
  TEST_ASSERT_FALSE(tokEq("a-long-enough-token", "a-long-enough-token!"));
  TEST_ASSERT_FALSE(tokEq("a-long-enough-token", ""));
}

void test_unprovisioned_token_never_matches(void) {
  TEST_ASSERT_FALSE(tokEq("", ""));
  TEST_ASSERT_FALSE(tokEq("", "anything"));
}

// Regression: the old compare folded the length difference in as a uint8_t,
// so a header exactly 256 bytes longer, with the token as its prefix, passed.
void test_token_prefix_plus_256_bytes_rejected(void) {
  const char *want = "a-long-enough-token";
  char got[300];
  const size_t wl = strlen(want);
  memcpy(got, want, wl);
  memset(got + wl, 'z', 256);
  TEST_ASSERT_FALSE(credTokenEqual(want, wl, got, wl + 256));
}

// --- Display / set checks ---------------------------------------------------

void test_describe_secrets_show_length_never_a_hash(void) {
  char d[CRED_DESC_LEN];
  const CredField secrets[] = {CRED_PASS, CRED_MQTT_PASS, CRED_TOKEN};
  for (CredField f : secrets) {
    TEST_ASSERT_TRUE(credIsSecret(f));
    credDescribe(f, "hunter2-hunter2", 15, d);
    TEST_ASSERT_EQUAL_STRING("set, 15 chars", d);
    TEST_ASSERT_NULL(strstr(d, "sha256"));
  }
}

void test_describe_public_fields_use_sha256(void) {
  char d[CRED_DESC_LEN];
  TEST_ASSERT_FALSE(credIsSecret(CRED_SSID));
  credDescribe(CRED_SSID, "abc", 3, d);
  TEST_ASSERT_EQUAL_STRING("sha256:ba7816bf", d);
  credDescribe(CRED_MQTT_USER, "abc", 3, d);
  TEST_ASSERT_EQUAL_STRING("sha256:ba7816bf", d);
  credDescribe(CRED_CA, kPem, strlen(kPem), d);
  TEST_ASSERT_EQUAL_STRING("sha256:4c0166be", d);
}

void test_check_set_pairs(void) {
  bool p[CRED_FIELD_COUNT] = {false};
  CredField bad = CRED_NONE;
  TEST_ASSERT_EQUAL(CRED_OK, credCheckSet(p, &bad));          // empty is valid
  p[CRED_TOKEN] = true;
  p[CRED_CA] = true;
  TEST_ASSERT_EQUAL(CRED_OK, credCheckSet(p, &bad));          // singles ok
  p[CRED_SSID] = true;
  TEST_ASSERT_EQUAL(CRED_E_PAIR, credCheckSet(p, &bad));
  TEST_ASSERT_EQUAL(CRED_PASS, bad);
  p[CRED_PASS] = true;
  TEST_ASSERT_EQUAL(CRED_OK, credCheckSet(p, &bad));
  p[CRED_MQTT_PASS] = true;
  TEST_ASSERT_EQUAL(CRED_E_PAIR, credCheckSet(p, &bad));
  TEST_ASSERT_EQUAL(CRED_MQTT_USER, bad);
}

void test_parse_values_are_literal(void) {
  // Leading/trailing spaces and quotes are part of the value.
  CredParsed p = parse("cred set pass  \"quoted pass\" \r\n");
  TEST_ASSERT_EQUAL(CRED_CMD_SET, p.cmd);
  TEST_ASSERT_EQUAL(15, p.valueLen);
  TEST_ASSERT_EQUAL_MEMORY(" \"quoted pass\" ", p.value, 15);
  TEST_ASSERT_EQUAL(CRED_OK, credValidate(CRED_PASS, p.value, p.valueLen));
  p = parse("cred set ssid it's here");
  TEST_ASSERT_EQUAL_MEMORY("it's here", p.value, 9);
}

void test_parse_commit_abort(void) {
  TEST_ASSERT_EQUAL(CRED_CMD_COMMIT, parse("cred commit").cmd);
  TEST_ASSERT_EQUAL(CRED_CMD_ABORT, parse("cred abort\r").cmd);
  TEST_ASSERT_EQUAL(CRED_CMD_BAD, parse("cred commit now").cmd);
}

void test_longest_set_line_fits_the_console_line(void) {
  // "cred set mqtt_pass " + a max-length value must not overflow CRED_LINE_MAX.
  TEST_ASSERT_TRUE(strlen("cred set mqtt_pass ") + CRED_MQTT_PASS_MAX <= CRED_LINE_MAX);
  TEST_ASSERT_TRUE(strlen("cred set token ") + CRED_TOKEN_MAX <= CRED_LINE_MAX);
}

void test_ca_over_cap_rejected(void) {
  static char big[CRED_CA_MAX + 64];
  memset(big, 'A', sizeof(big) - 1);
  big[sizeof(big) - 1] = 0;
  TEST_ASSERT_EQUAL(CRED_E_LONG, v(CRED_CA, big));
}

// --- Commit state -------------------------------------------------------------

void test_never_provisioned_is_not_incomplete(void) {
  // A fresh board: no `complete` key, no fields. Not an interrupted commit.
  TEST_ASSERT_EQUAL(CRED_COMMIT_NONE, credCommitState(false, 0, false));
  TEST_ASSERT_EQUAL_STRING("not provisioned",
                           credCommitStateName(credCommitState(false, 0, false)));
}

void test_interrupted_commit_is_incomplete(void) {
  // complete=0 is written first: cut before any field, or after some.
  TEST_ASSERT_EQUAL(CRED_COMMIT_INCOMPLETE, credCommitState(true, 0, false));
  TEST_ASSERT_EQUAL(CRED_COMMIT_INCOMPLETE, credCommitState(true, 0, true));
  // Fields with no flag at all were not written by a finished commit.
  TEST_ASSERT_EQUAL(CRED_COMMIT_INCOMPLETE, credCommitState(false, 0, true));
  TEST_ASSERT_EQUAL_STRING("INCOMPLETE",
                           credCommitStateName(CRED_COMMIT_INCOMPLETE));
}

void test_finished_commit_is_complete(void) {
  TEST_ASSERT_EQUAL(CRED_COMMIT_COMPLETE, credCommitState(true, 1, true));
  // `cred clear all` + commit: a finished commit of an empty set.
  TEST_ASSERT_EQUAL(CRED_COMMIT_COMPLETE, credCommitState(true, 1, false));
  TEST_ASSERT_EQUAL_STRING("complete", credCommitStateName(CRED_COMMIT_COMPLETE));
}

void test_wipe_zeroes_every_byte(void) {
  uint8_t b[37];
  memset(b, 0xA5, sizeof(b));
  credWipe(b, sizeof(b));
  for (size_t i = 0; i < sizeof(b); i++) TEST_ASSERT_EQUAL_HEX8(0, b[i]);
  credWipe(b, 0);                        // n == 0 is a no-op, not a crash
}

// --- hub address --------------------------------------------------------------

static bool ip(const char *s, uint8_t o[4]) { return credParseIPv4(s, strlen(s), o); }

void test_hub_addr_parses_dotted_quad(void) {
  uint8_t o[4];
  TEST_ASSERT_TRUE(ip("192.168.137.1", o));
  TEST_ASSERT_EQUAL_UINT8(192, o[0]);
  TEST_ASSERT_EQUAL_UINT8(1, o[3]);
  TEST_ASSERT_TRUE(ip("10.42.0.1", o));
  TEST_ASSERT_EQUAL(CRED_OK, v(CRED_HUB_ADDR, "10.42.0.1"));
}

void test_hub_addr_rejects(void) {
  uint8_t o[4];
  const char *bad[] = {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.04",
                       "a.b.c.d", "1..2.3", "1.2.3.4 ", " 1.2.3.4", "0.0.0.0",
                       "255.255.255.255", "1.2.3.-4", "hub.local"};
  for (const char *b : bad) {
    TEST_ASSERT_FALSE_MESSAGE(ip(b, o), b);
    if (*b) TEST_ASSERT_NOT_EQUAL(CRED_OK, v(CRED_HUB_ADDR, b));
  }
}

void test_hub_name_label_rules(void) {
  TEST_ASSERT_EQUAL(CRED_OK, v(CRED_HUB_NAME, "carhub"));
  TEST_ASSERT_EQUAL(CRED_OK, v(CRED_HUB_NAME, "hub-2"));
  const char *bad[] = {"carhub.local", "CarHub", "-hub", "hub-", "car hub",
                       "car_hub", "abcdefghijklmnopqrstuvwxyz0123456"};
  for (const char *b : bad) TEST_ASSERT_NOT_EQUAL_MESSAGE(CRED_OK, v(CRED_HUB_NAME, b), b);
  TEST_ASSERT_NOT_EQUAL(CRED_OK, v(CRED_HUB_NAME, ""));
  const CredParsed p = parse("cred set hub_name carhub");
  TEST_ASSERT_EQUAL(CRED_CMD_SET, p.cmd);
  TEST_ASSERT_EQUAL(CRED_HUB_NAME, p.field);
  TEST_ASSERT_FALSE(credIsSecret(CRED_HUB_NAME));
  char d[CRED_DESC_LEN];
  credDescribe(CRED_HUB_NAME, "carhub", 6, d);
  TEST_ASSERT_EQUAL_STRING("carhub", d);
}

// logger_name follows exactly the hub_name label rules.
void test_logger_name_label_rules(void) {
  TEST_ASSERT_EQUAL(CRED_OK, v(CRED_LOGGER_NAME, "cardiag"));
  TEST_ASSERT_EQUAL(CRED_OK, v(CRED_LOGGER_NAME, "cardiag-2"));
  const char *bad[] = {"cardiag.local", "CarDiag", "-cardiag", "cardiag-",
                       "car diag", "car_diag", "abcdefghijklmnopqrstuvwxyz0123456", ""};
  for (const char *b : bad)
    TEST_ASSERT_NOT_EQUAL_MESSAGE(CRED_OK, v(CRED_LOGGER_NAME, b), b);
  const CredParsed p = parse("cred set logger_name cardiag");
  TEST_ASSERT_EQUAL(CRED_CMD_SET, p.cmd);
  TEST_ASSERT_EQUAL(CRED_LOGGER_NAME, p.field);
  TEST_ASSERT_FALSE(credIsSecret(CRED_LOGGER_NAME));
  TEST_ASSERT_EQUAL_STRING("logger_name", credFieldName(CRED_LOGGER_NAME));
  TEST_ASSERT_TRUE(strlen(credNvsKey(CRED_LOGGER_NAME)) <= 15);   // NVS key limit
  char d[CRED_DESC_LEN];
  credDescribe(CRED_LOGGER_NAME, "cardiag", 7, d);
  TEST_ASSERT_EQUAL_STRING("cardiag", d);
  // The longest set line still fits the console.
  TEST_ASSERT_TRUE(strlen("cred set logger_name ") + CRED_LOGGER_NAME_MAX <= CRED_LINE_MAX);
}

void test_hub_addr_is_a_staged_field_shown_as_itself(void) {
  const CredParsed p = parse("cred set hub_addr 192.168.137.1");
  TEST_ASSERT_EQUAL(CRED_CMD_SET, p.cmd);
  TEST_ASSERT_EQUAL(CRED_HUB_ADDR, p.field);
  TEST_ASSERT_FALSE(credIsSecret(CRED_HUB_ADDR));
  char d[CRED_DESC_LEN];
  credDescribe(CRED_HUB_ADDR, "192.168.137.1", 13, d);
  TEST_ASSERT_EQUAL_STRING("192.168.137.1", d);
  TEST_ASSERT_EQUAL(CRED_CMD_CLEAR, parse("cred clear hub_addr").cmd);
  // Unpaired: a hub address alone is a valid set.
  bool present[CRED_FIELD_COUNT] = {false};
  present[CRED_HUB_ADDR] = true;
  TEST_ASSERT_EQUAL(CRED_OK, credCheckSet(present, nullptr));
  TEST_ASSERT_TRUE(strlen(credNvsKey(CRED_HUB_ADDR)) <= 15);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_sha256_known_vectors);
  RUN_TEST(test_fingerprint_matches_sha256sum_prefix);
  RUN_TEST(test_fingerprint_never_contains_the_value);
  RUN_TEST(test_ca_fingerprint_matches_openssl);
  RUN_TEST(test_pem_crlf_and_no_trailing_newline_ok);
  RUN_TEST(test_pem_rejects_malformed);
  RUN_TEST(test_pem_rejects_two_certs_and_junk_outside);
  RUN_TEST(test_ssid_limits);
  RUN_TEST(test_wpa_pass_limits);
  RUN_TEST(test_placeholders_rejected_for_secrets);
  RUN_TEST(test_token_and_mqtt);
  RUN_TEST(test_parse_set_value_keeps_inner_spaces_and_drops_crlf);
  RUN_TEST(test_parse_set_every_field);
  RUN_TEST(test_parse_set_empty_value_is_a_set_that_validation_rejects);
  RUN_TEST(test_parse_other_commands);
  RUN_TEST(test_parse_rejects);
  RUN_TEST(test_token_equal);
  RUN_TEST(test_unprovisioned_token_never_matches);
  RUN_TEST(test_token_prefix_plus_256_bytes_rejected);
  RUN_TEST(test_describe_secrets_show_length_never_a_hash);
  RUN_TEST(test_describe_public_fields_use_sha256);
  RUN_TEST(test_check_set_pairs);
  RUN_TEST(test_parse_values_are_literal);
  RUN_TEST(test_parse_commit_abort);
  RUN_TEST(test_longest_set_line_fits_the_console_line);
  RUN_TEST(test_ca_over_cap_rejected);
  RUN_TEST(test_never_provisioned_is_not_incomplete);
  RUN_TEST(test_interrupted_commit_is_incomplete);
  RUN_TEST(test_finished_commit_is_complete);
  RUN_TEST(test_wipe_zeroes_every_byte);
  RUN_TEST(test_hub_addr_parses_dotted_quad);
  RUN_TEST(test_hub_addr_rejects);
  RUN_TEST(test_hub_addr_is_a_staged_field_shown_as_itself);
  RUN_TEST(test_hub_name_label_rules);
  RUN_TEST(test_logger_name_label_rules);
  return UNITY_END();
}

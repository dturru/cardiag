// hubaddr.h: hub address selection order, fallback, and re-resolve backoff.

#include <unity.h>

#include "hubaddr.h"

void setUp(void) {}
void tearDown(void) {}

static uint32_t ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | d;
}

static const uint32_t MDNS = 0xC0A88905;   // 192.168.137.5
static const uint32_t OLD  = 0xC0A88963;   // 192.168.137.99
static const uint32_t NVS  = 0xC0A88902;
static const uint32_t GW   = 0xC0A88901;

void test_selection_order(void) {
  HubAddrChoice c = hubAddrSelect({MDNS, OLD, NVS, GW});
  TEST_ASSERT_EQUAL_HEX32(MDNS, c.ip);
  TEST_ASSERT_EQUAL(HUBADDR_MDNS, c.source);
  c = hubAddrSelect({0, OLD, NVS, GW});            // query failed: cache
  TEST_ASSERT_EQUAL_HEX32(OLD, c.ip);
  TEST_ASSERT_EQUAL(HUBADDR_MDNS_CACHE, c.source);
  c = hubAddrSelect({0, 0, NVS, GW});              // never resolved: override
  TEST_ASSERT_EQUAL(HUBADDR_NVS, c.source);
  c = hubAddrSelect({0, 0, 0, GW});                // hub's own AP
  TEST_ASSERT_EQUAL_HEX32(GW, c.ip);
  TEST_ASSERT_EQUAL(HUBADDR_GATEWAY, c.source);
  c = hubAddrSelect({0, 0, 0, 0});
  TEST_ASSERT_EQUAL(HUBADDR_NONE, c.source);
  TEST_ASSERT_EQUAL_HEX32(0, c.ip);
}

// ⭐ The bench case: the hub moved. A fresh mDNS answer wins over a stale
// hub_addr in NVS without anyone reprovisioning.
void test_fresh_mdns_beats_a_stale_nvs_address(void) {
  const HubAddrChoice c = hubAddrSelect({ip(192, 168, 137, 44), 0, ip(192, 168, 137, 12), GW});
  TEST_ASSERT_EQUAL_HEX32(ip(192, 168, 137, 44), c.ip);
  TEST_ASSERT_EQUAL(HUBADDR_MDNS, c.source);
}

void test_broadcast_and_zero_are_never_chosen(void) {
  const HubAddrChoice c = hubAddrSelect({0xFFFFFFFFu, 0, 0xFFFFFFFFu, GW});
  TEST_ASSERT_EQUAL(HUBADDR_GATEWAY, c.source);
}

void test_source_names(void) {
  TEST_ASSERT_EQUAL_STRING("mdns", hubAddrSourceName(HUBADDR_MDNS));
  TEST_ASSERT_EQUAL_STRING("mdns-cache", hubAddrSourceName(HUBADDR_MDNS_CACHE));
  TEST_ASSERT_EQUAL_STRING("nvs", hubAddrSourceName(HUBADDR_NVS));
  TEST_ASSERT_EQUAL_STRING("gateway", hubAddrSourceName(HUBADDR_GATEWAY));
  TEST_ASSERT_EQUAL_STRING("none", hubAddrSourceName(HUBADDR_NONE));
}

// --- scheduler -------------------------------------------------------------------

static const uint32_t BASE = 2000, MAXB = 60000, GAP = 5000;

void test_kick_then_due_then_success_parks(void) {
  HubResolveSched s;
  hubResolveReset(s);
  TEST_ASSERT_FALSE(hubResolveDue(s, 0));
  hubResolveKick(s, 100, GAP);
  TEST_ASSERT_TRUE(hubResolveDue(s, 100));
  hubResolveStarted(s, 100);
  TEST_ASSERT_FALSE(hubResolveDue(s, 200));         // one query at a time
  hubResolveKick(s, 300, GAP);                      // ignored while in flight
  hubResolveFinished(s, 2100, true, BASE, MAXB);
  TEST_ASSERT_FALSE(hubResolveDue(s, 999999));      // parked
}

void test_failures_back_off_and_cap(void) {
  HubResolveSched s;
  hubResolveReset(s);
  hubResolveKick(s, 0, GAP);
  uint32_t now = 0;
  const uint32_t expect[] = {2000, 4000, 8000, 16000, 32000, 60000, 60000};
  for (uint32_t e : expect) {
    TEST_ASSERT_TRUE(hubResolveDue(s, now));
    hubResolveStarted(s, now);
    now += 2000;                                    // the query's own timeout
    hubResolveFinished(s, now, false, BASE, MAXB);
    TEST_ASSERT_FALSE(hubResolveDue(s, now + e - 1));
    TEST_ASSERT_TRUE(hubResolveDue(s, now + e));
    now += e;
  }
  // A success resets the count.
  hubResolveStarted(s, now);
  hubResolveFinished(s, now + 500, true, BASE, MAXB);
  TEST_ASSERT_EQUAL_UINT8(0, s.fails);
}

void test_kick_does_not_jump_a_running_backoff(void) {
  HubResolveSched s;
  hubResolveReset(s);
  hubResolveKick(s, 0, GAP);
  hubResolveStarted(s, 0);
  hubResolveFinished(s, 2000, false, BASE, MAXB);   // next at 4000
  hubResolveKick(s, 2500, GAP);                     // "hub silent" again
  TEST_ASSERT_FALSE(hubResolveDue(s, 3999));
  TEST_ASSERT_TRUE(hubResolveDue(s, 4000));
}

void test_kicks_rate_limited_after_a_success(void) {
  HubResolveSched s;
  hubResolveReset(s);
  hubResolveKick(s, 0, GAP);
  hubResolveStarted(s, 0);
  hubResolveFinished(s, 300, true, BASE, MAXB);
  hubResolveKick(s, 1000, GAP);                     // within minGap of the start
  TEST_ASSERT_FALSE(hubResolveDue(s, 4999));
  TEST_ASSERT_TRUE(hubResolveDue(s, 5000));
}

void test_hub_silence(void) {
  // Link up at 1000, never seen: silent 90 s after link-up.
  TEST_ASSERT_FALSE(hubSilent(90999, 0, 1000, 90000));
  TEST_ASSERT_TRUE(hubSilent(91000, 0, 1000, 90000));
  // Seen at 50000: counts from there.
  TEST_ASSERT_FALSE(hubSilent(139999, 50000, 1000, 90000));
  TEST_ASSERT_TRUE(hubSilent(140000, 50000, 1000, 90000));
  // A request seen before the CURRENT link came up does not count.
  TEST_ASSERT_FALSE(hubSilent(95000, 500, 10000, 90000));
}

void test_scheduler_across_millis_wrap(void) {
  HubResolveSched s;
  hubResolveReset(s);
  hubResolveKick(s, 0xFFFFF000u, GAP);
  hubResolveStarted(s, 0xFFFFF000u);
  hubResolveFinished(s, 0xFFFFFF00u, false, BASE, MAXB);   // next = 0x6D0, wrapped
  TEST_ASSERT_FALSE(hubResolveDue(s, 0xFFFFFFF0u));
  TEST_ASSERT_FALSE(hubResolveDue(s, 0x000006CFu));
  TEST_ASSERT_TRUE(hubResolveDue(s, 0x000006D0u));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_selection_order);
  RUN_TEST(test_fresh_mdns_beats_a_stale_nvs_address);
  RUN_TEST(test_broadcast_and_zero_are_never_chosen);
  RUN_TEST(test_source_names);
  RUN_TEST(test_kick_then_due_then_success_parks);
  RUN_TEST(test_failures_back_off_and_cap);
  RUN_TEST(test_kick_does_not_jump_a_running_backoff);
  RUN_TEST(test_kicks_rate_limited_after_a_success);
  RUN_TEST(test_hub_silence);
  RUN_TEST(test_scheduler_across_millis_wrap);
  return UNITY_END();
}

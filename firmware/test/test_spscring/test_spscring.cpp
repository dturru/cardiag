// spscring.h: the change log's producer (canTask) never waits.
//
// The key case: the consumer is mid "compaction" -- holding its own lock for a
// long time, as the old memmove did -- and the producer keeps appending at
// full speed without ever blocking. Real threads, not a simulation.

#include <unity.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "spscring.h"

using Clock = std::chrono::steady_clock;

void setUp(void) {}
void tearDown(void) {}

void test_capacity_rounds_down_to_a_power_of_two(void) {
  SpscRing r;
  spscInit(&r, 131072);
  TEST_ASSERT_EQUAL_UINT32(131072, r.cap);
  spscInit(&r, 100000);
  TEST_ASSERT_EQUAL_UINT32(65536, r.cap);
  spscInit(&r, 0);
  uint32_t s;
  TEST_ASSERT_FALSE(spscReserve(&r, &s));        // disabled: every push drops
}

void test_full_ring_refuses_without_waiting_then_recovers(void) {
  SpscRing r;
  spscInit(&r, 8);
  uint32_t s;
  for (int i = 0; i < 8; i++) {
    TEST_ASSERT_TRUE(spscReserve(&r, &s));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)i, s);
    spscPublish(&r);
  }
  TEST_ASSERT_FALSE(spscReserve(&r, &s));        // full: a counted drop
  TEST_ASSERT_EQUAL_UINT32(8, spscCount(&r));
  TEST_ASSERT_EQUAL_UINT32(3, spscConsume(&r, 3));
  TEST_ASSERT_TRUE(spscReserve(&r, &s));
  TEST_ASSERT_EQUAL_UINT32(0, s);                 // wrapped to the freed slot
}

void test_indices_stay_continuous_across_the_32_bit_wrap(void) {
  SpscRing r;
  spscInit(&r, 16);
  r.head.store(0xFFFFFFF8u);
  r.tail.store(0xFFFFFFF8u);
  uint32_t s, prev = 0;
  for (int i = 0; i < 16; i++) {
    TEST_ASSERT_TRUE(spscReserve(&r, &s));
    if (i) TEST_ASSERT_EQUAL_UINT32((prev + 1) & 15, s);
    prev = s;
    spscPublish(&r);
  }
  TEST_ASSERT_EQUAL_UINT32(16, spscCount(&r));
  TEST_ASSERT_FALSE(spscReserve(&r, &s));
}

void test_producer_never_waits_while_consumer_holds_its_lock(void) {
  // Consumer side: takes its lock and holds it 300 ms, as the old compaction
  // memmove did (and far longer than the TWAI queue lasts). The producer
  // appends 20,000 rows meanwhile. It must never take that lock, never wait,
  // and every append must fit or be counted -- none can block.
  static uint32_t data[32768];
  SpscRing r;
  spscInit(&r, 32768);
  std::mutex consumerLock;
  std::atomic<bool> holding{false};
  std::atomic<uint32_t> drops{0};

  std::thread consumer([&] {
    std::lock_guard<std::mutex> g(consumerLock);   // "compaction in progress"
    holding = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    holding = false;
  });
  while (!holding) std::this_thread::yield();

  auto worst = Clock::duration::zero();
  uint32_t duringHold = 0;
  for (uint32_t i = 0; i < 20000; i++) {
    const auto t0 = Clock::now();
    uint32_t slot;
    if (spscReserve(&r, &slot)) {
      data[slot] = i;
      spscPublish(&r);
    } else {
      drops++;
    }
    const auto d = Clock::now() - t0;
    if (d > worst) worst = d;
    if (holding) duringHold++;
  }
  consumer.join();

  TEST_ASSERT_TRUE(duringHold > 0);               // really overlapped the hold
  TEST_ASSERT_EQUAL_UINT32(0, drops.load());
  // No append came anywhere near the 300 ms hold: it never waited on it.
  TEST_ASSERT_TRUE(worst < std::chrono::milliseconds(50));
  // And what the producer wrote is what the consumer reads, in order.
  TEST_ASSERT_EQUAL_UINT32(20000, spscCount(&r));
  for (uint32_t i = 0; i < 20000; i++) {
    TEST_ASSERT_EQUAL_UINT32(i, data[spscSlot(&r, i)]);
  }
}

void test_concurrent_producer_and_consumer_lose_nothing(void) {
  static uint32_t data[1024];
  SpscRing r;
  spscInit(&r, 1024);
  const uint32_t N = 200000;
  std::atomic<uint32_t> drops{0};
  std::thread producer([&] {
    for (uint32_t i = 0; i < N;) {
      uint32_t slot;
      if (spscReserve(&r, &slot)) {
        data[slot] = i++;
        spscPublish(&r);
      } else {
        drops++;                                  // counted, then retried here
        std::this_thread::yield();
      }
    }
  });
  uint32_t expect = 0;
  bool inOrder = true;
  while (expect < N) {
    const uint32_t n = spscCount(&r);
    for (uint32_t i = 0; i < n; i++) {
      if (data[spscSlot(&r, i)] != expect + i) inOrder = false;
    }
    spscConsume(&r, n);
    expect += n;
  }
  producer.join();
  TEST_ASSERT_TRUE(inOrder);
  TEST_ASSERT_EQUAL_UINT32(0, spscCount(&r));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_capacity_rounds_down_to_a_power_of_two);
  RUN_TEST(test_full_ring_refuses_without_waiting_then_recovers);
  RUN_TEST(test_indices_stay_continuous_across_the_32_bit_wrap);
  RUN_TEST(test_producer_never_waits_while_consumer_holds_its_lock);
  RUN_TEST(test_concurrent_producer_and_consumer_lose_nothing);
  return UNITY_END();
}

/*
 * Spektrafilm Android -- concurrent lossless-JPEG first-use regression.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
#include "public_api_test_support.h"

#include <libraw/libraw.h>

#include <array>
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

int main() {
  constexpr std::size_t kThreadCount = 8U;
  const std::vector<std::uint8_t> fixture =
      sfraw::hosttest::makeSof1ZrlDng();
  std::atomic<std::size_t> ready{0U};
  std::atomic<bool> start{false};
  std::array<sfraw::hosttest::DecodeOutcome, kThreadCount> outcomes{};
  std::vector<std::thread> workers;
  workers.reserve(kThreadCount);
  for (std::size_t index = 0U; index < kThreadCount; ++index) {
    workers.emplace_back([&, index]() {
      ready.fetch_add(1U, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      outcomes[index] = sfraw::hosttest::exercisePublicDecode(fixture);
    });
  }
  while (ready.load(std::memory_order_acquire) != kThreadCount) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  for (std::thread& worker : workers) worker.join();

  for (std::size_t index = 0U; index < kThreadCount; ++index) {
    const auto& outcome = outcomes[index];
    if (outcome.openCode != LIBRAW_SUCCESS ||
        outcome.unpackCode == LIBRAW_SUCCESS ||
        outcome.terminalStage != sfraw::hosttest::DecodeStage::kUnpack) {
      std::cerr << "thread " << index << " failed: "
                << sfraw::hosttest::describe(outcome) << '\n';
      return 1;
    }
  }
  std::cout << "8 concurrent SOF1 first-use decodes rejected safely\n";
  return 0;
}

/*
 * Spektrafilm for Android -- native RAW result ownership registry.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
#include "native_allocation_registry.h"

#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace sfraw {

NativeAllocationRegistry::~NativeAllocationRegistry() {
  std::unordered_map<std::uint64_t, Allocation> remaining;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    remaining.swap(allocations_);
  }
  for (const auto& entry : remaining) std::free(entry.second.base);
}

std::uint64_t NativeAllocationRegistry::adopt(void* base,
                                              std::size_t capacity) {
  if (base == nullptr || capacity == 0U) {
    throw std::invalid_argument("cannot adopt an empty native allocation");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  std::uint64_t token;
  do {
    token = next_token_++;
  } while (token == 0U || allocations_.find(token) != allocations_.end());
  allocations_.emplace(token, Allocation{base, capacity});
  return token;
}

ReleaseResult NativeAllocationRegistry::release(std::uint64_t token,
                                                 void* base,
                                                 std::size_t capacity) {
  void* released = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = allocations_.find(token);
    if (found == allocations_.end()) return ReleaseResult::UnknownToken;
    if (found->second.base != base || found->second.capacity != capacity) {
      return ReleaseResult::Mismatch;
    }
    released = found->second.base;
    allocations_.erase(found);
  }
  std::free(released);
  return ReleaseResult::Released;
}

std::size_t NativeAllocationRegistry::outstanding() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return allocations_.size();
}

NativeAllocationRegistry& nativeAllocationRegistry() {
  static NativeAllocationRegistry registry;
  return registry;
}

NativeCancellationRegistry::~NativeCancellationRegistry() {
  std::unordered_map<std::uint64_t, std::shared_ptr<Flag>> remaining;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    remaining.swap(flags_);
  }
  for (const auto& entry : remaining) {
    entry.second->store(true, std::memory_order_release);
  }
}

std::uint64_t NativeCancellationRegistry::create() {
  auto flag = std::make_shared<Flag>(false);
  std::lock_guard<std::mutex> lock(mutex_);
  std::uint64_t token;
  do {
    token = next_token_++;
  } while (token == 0U || flags_.find(token) != flags_.end());
  flags_.emplace(token, std::move(flag));
  return token;
}

std::shared_ptr<NativeCancellationRegistry::Flag>
NativeCancellationRegistry::acquire(std::uint64_t token) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = flags_.find(token);
  return found == flags_.end() ? nullptr : found->second;
}

bool NativeCancellationRegistry::cancel(std::uint64_t token) {
  const auto flag = acquire(token);
  if (flag == nullptr) return false;
  flag->store(true, std::memory_order_release);
  return true;
}

bool NativeCancellationRegistry::release(std::uint64_t token) {
  std::shared_ptr<Flag> released;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = flags_.find(token);
    if (found == flags_.end()) return false;
    released = std::move(found->second);
    flags_.erase(found);
  }
  released->store(true, std::memory_order_release);
  return true;
}

std::size_t NativeCancellationRegistry::outstanding() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return flags_.size();
}

NativeCancellationRegistry& nativeCancellationRegistry() {
  static NativeCancellationRegistry registry;
  return registry;
}

}  // namespace sfraw

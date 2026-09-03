/*
 * Spektrafilm for Android -- native RAW result ownership registry.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
#ifndef SFRAW_NATIVE_ALLOCATION_REGISTRY_H
#define SFRAW_NATIVE_ALLOCATION_REGISTRY_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace sfraw {

enum class ReleaseResult {
  Released,
  UnknownToken,
  Mismatch,
};

/**
 * Owns malloc-compatible allocations behind opaque generation tokens.
 *
 * Release requires the token, exact base address, and exact capacity recorded
 * at adoption. This makes foreign buffers, interior slices, stale generations,
 * repeated closes, and concurrent closes safe rejections instead of free(3)
 * misuse. The registry frees any still-owned allocation at destruction.
 */
class NativeAllocationRegistry final {
 public:
  NativeAllocationRegistry() = default;
  ~NativeAllocationRegistry();

  NativeAllocationRegistry(const NativeAllocationRegistry&) = delete;
  NativeAllocationRegistry& operator=(const NativeAllocationRegistry&) = delete;

  /** Adopt a non-null, non-empty malloc-compatible allocation. */
  std::uint64_t adopt(void* base, std::size_t capacity);

  /** Release only an exact token/base/capacity match. Thread-safe. */
  ReleaseResult release(std::uint64_t token, void* base,
                        std::size_t capacity);

  std::size_t outstanding() const;

 private:
  struct Allocation {
    void* base;
    std::size_t capacity;
  };

  mutable std::mutex mutex_;
  std::unordered_map<std::uint64_t, Allocation> allocations_;
  std::uint64_t next_token_ = 1U;
};

/** Process registry used by the JNI bridge. */
NativeAllocationRegistry& nativeAllocationRegistry();

/** Opaque-token registry whose shared leases keep cancellation flags alive. */
class NativeCancellationRegistry final {
 public:
  using Flag = std::atomic<bool>;

  NativeCancellationRegistry() = default;
  ~NativeCancellationRegistry();

  NativeCancellationRegistry(const NativeCancellationRegistry&) = delete;
  NativeCancellationRegistry& operator=(const NativeCancellationRegistry&) = delete;

  std::uint64_t create();
  std::shared_ptr<Flag> acquire(std::uint64_t token) const;
  bool cancel(std::uint64_t token);
  /** Removes the generation and cancels any already-acquired decode lease. */
  bool release(std::uint64_t token);
  std::size_t outstanding() const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::uint64_t, std::shared_ptr<Flag>> flags_;
  std::uint64_t next_token_ = 1U;
};

NativeCancellationRegistry& nativeCancellationRegistry();

}  // namespace sfraw

#endif  // SFRAW_NATIVE_ALLOCATION_REGISTRY_H

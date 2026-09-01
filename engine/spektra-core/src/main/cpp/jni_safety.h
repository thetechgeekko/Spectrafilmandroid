#ifndef SPEKTRA_JNI_SAFETY_H
#define SPEKTRA_JNI_SAFETY_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "runtime/memory_budget.h"

namespace spk::jni {

using CancellationCheck = int (*)(void*);

// Copy a potentially large JNI result in bounded chunks. Cancellation is
// sampled before each chunk and once after the final chunk, so the caller can
// discard a partial destination instead of publishing a stale successful result.
inline bool copy_bytes_cancellable(
        void* destination, const void* source, std::size_t byte_count,
        CancellationCheck cancellation, void* cancellation_context,
        std::size_t chunk_size = 1024U * 1024U) noexcept {
    if (!destination || !source || byte_count == 0U || chunk_size == 0U) {
        return false;
    }
    auto* out = static_cast<std::uint8_t*>(destination);
    const auto* in = static_cast<const std::uint8_t*>(source);
    std::size_t offset = 0U;
    while (offset < byte_count) {
        if (cancellation && cancellation(cancellation_context) != 0) return false;
        const std::size_t remaining = byte_count - offset;
        const std::size_t copied = remaining < chunk_size ? remaining : chunk_size;
        std::memcpy(out + offset, in + offset, copied);
        offset += copied;
    }
    return !cancellation || cancellation(cancellation_context) == 0;
}

inline bool checked_rgb_f32_bytes(std::int32_t width, std::int32_t height,
                                  std::uint64_t* out) noexcept {
    if (!out || width <= 0 || height <= 0) return false;
    constexpr std::uint64_t kBytesPerPixel = 3u * sizeof(float);
    const auto w = static_cast<std::uint64_t>(width);
    const auto h = static_cast<std::uint64_t>(height);
    if (w > std::numeric_limits<std::uint64_t>::max() / h) return false;
    const std::uint64_t pixels = w * h;
    if (pixels > std::numeric_limits<std::uint64_t>::max() / kBytesPerPixel) {
        return false;
    }
    *out = pixels * kBytesPerPixel;
    return true;
}

inline bool checked_float_buffer_range(std::int64_t capacity,
                                       std::int64_t position,
                                       std::int64_t limit,
                                       std::uint64_t required,
                                       std::uintptr_t base,
                                       std::uintptr_t* address) noexcept {
    if (!address || capacity < 0 || position < 0 || limit < position ||
        limit > capacity) {
        return false;
    }
    const auto remaining = static_cast<std::uint64_t>(limit - position);
    if (remaining < required ||
        (position % static_cast<std::int64_t>(alignof(float))) != 0) {
        return false;
    }
    const auto offset = static_cast<std::uintptr_t>(position);
    if (base > std::numeric_limits<std::uintptr_t>::max() - offset) return false;
    const auto resolved = base + offset;
    if ((resolved % alignof(float)) != 0) return false;
    *address = resolved;
    return true;
}

// Move-only authority returned by AllocationRegistry::take. The budget
// reservation deliberately remains alive until this object is destroyed, so a
// caller can free the allocation first and only then release accounting.
class TakenAllocation final {
public:
    TakenAllocation() noexcept = default;
    TakenAllocation(void* base, std::size_t size,
                    spk::memory::MemoryReservation reservation) noexcept
        : base_(base), size_(size), reservation_(std::move(reservation)) {}

    TakenAllocation(const TakenAllocation&) = delete;
    TakenAllocation& operator=(const TakenAllocation&) = delete;
    TakenAllocation(TakenAllocation&&) noexcept = default;
    TakenAllocation& operator=(TakenAllocation&&) noexcept = default;

    explicit operator bool() const noexcept {
        return base_ != nullptr && static_cast<bool>(reservation_);
    }
    void* base() const noexcept { return base_; }
    std::size_t size() const noexcept { return size_; }

private:
    void* base_ = nullptr;
    std::size_t size_ = 0;
    spk::memory::MemoryReservation reservation_;
};

// Tracks only buffers allocated by this JNI bridge. `take` requires the exact
// base, capacity, and opaque token, so a slice/interior pointer, foreign direct
// buffer, or a stale/wrong owner token can never reach free(). Removal transfers
// both free authority and the still-live budget reservation to TakenAllocation.
class AllocationRegistry {
public:
    explicit AllocationRegistry(
            spk::memory::MemoryBudget& budget =
                spk::memory::process_memory_budget()) noexcept
        : budget_(&budget) {}

    spk::memory::MemoryReservation reserve(
            std::size_t size,
            spk::memory::MemoryStage stage =
                spk::memory::MemoryStage::Unknown) noexcept {
        if (size == 0) return {};
        return budget_->try_reserve(
            static_cast<std::uint64_t>(size),
            spk::memory::MemoryDomain::JniOwned, stage);
    }

    // Compatibility path for callers that already own an allocation. JNI paths
    // reserve before malloc and use add_reserved so the ceiling is never
    // transiently exceeded by an unadmitted buffer.
    bool add(void* base, std::size_t size, std::uint64_t* token,
             spk::memory::MemoryStage stage =
                 spk::memory::MemoryStage::Unknown) {
        auto reservation = reserve(size, stage);
        if (!reservation) return false;
        return add_reserved(base, size, token, reservation);
    }

    bool add_reserved(void* base, std::size_t size, std::uint64_t* token,
                      spk::memory::MemoryReservation& reservation) {
        if (!base || size == 0 || !token) return false;
        if (!reservation || reservation.bytes() != size ||
            reservation.domain() != spk::memory::MemoryDomain::JniOwned) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (allocations_.find(base) != allocations_.end()) return false;
        const std::uint64_t next = next_token_.fetch_add(1, std::memory_order_relaxed);
        if (next == 0 ||
            next > static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        // Allocate/publish the map node before moving the reservation. If node
        // allocation throws, the caller still holds admission while it frees
        // the malloc'd bytes in its catch path.
        const auto result = allocations_.emplace(
            base, Entry{size, next, spk::memory::MemoryReservation{}});
        if (!result.second) return false;
        result.first->second.reservation = std::move(reservation);
        *token = next;
        return true;
    }

    TakenAllocation take(void* base, std::size_t presented_size,
                         std::uint64_t token) {
        if (!base || presented_size == 0 || token == 0) return {};
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = allocations_.find(base);
        if (it == allocations_.end() || it->second.size != presented_size ||
            it->second.token != token) return {};
        TakenAllocation owned(
            base, it->second.size, std::move(it->second.reservation));
        allocations_.erase(it);
        return owned;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return allocations_.size();
    }

private:
    struct Entry {
        std::size_t size;
        std::uint64_t token;
        spk::memory::MemoryReservation reservation;
    };

    spk::memory::MemoryBudget* budget_;
    mutable std::mutex mutex_;
    std::unordered_map<void*, Entry> allocations_;
    std::atomic<std::uint64_t> next_token_{1};
};

// Accounts externally-owned memory (for example JVM byte arrays and Bitmaps)
// without pretending native code owns or may free the allocation itself. The
// opaque token carries only reservation authority; release is exact-once.
class ExternalReservationRegistry {
public:
    explicit ExternalReservationRegistry(
            spk::memory::MemoryDomain domain,
            spk::memory::MemoryBudget& budget =
                spk::memory::process_memory_budget()) noexcept
        : domain_(domain), budget_(&budget) {}

    std::uint64_t reserve(
            std::size_t size,
            spk::memory::MemoryStage stage =
                spk::memory::MemoryStage::Unknown) {
        if (size == 0) return 0;
        auto reservation = budget_->try_reserve(
            static_cast<std::uint64_t>(size), domain_, stage);
        if (!reservation) return 0;

        std::lock_guard<std::mutex> lock(mutex_);
        const std::uint64_t token =
            next_token_.fetch_add(1, std::memory_order_relaxed);
        if (token == 0 ||
            token > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())) {
            return 0;
        }
        const bool inserted = reservations_.emplace(
            token, std::move(reservation)).second;
        return inserted ? token : 0;
    }

    bool release(std::uint64_t token) {
        if (token == 0) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        return reservations_.erase(token) == 1;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return reservations_.size();
    }

private:
    spk::memory::MemoryDomain domain_;
    spk::memory::MemoryBudget* budget_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, spk::memory::MemoryReservation>
        reservations_;
    std::atomic<std::uint64_t> next_token_{1};
};

}  // namespace spk::jni

#endif  // SPEKTRA_JNI_SAFETY_H

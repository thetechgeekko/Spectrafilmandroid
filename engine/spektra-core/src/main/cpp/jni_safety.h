#ifndef SPEKTRA_JNI_SAFETY_H
#define SPEKTRA_JNI_SAFETY_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <atomic>
#include <mutex>
#include <unordered_map>

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

// Tracks only buffers allocated by this JNI bridge. `take` requires the exact
// base, capacity, and opaque token, so a slice/interior pointer, foreign direct
// buffer, or a stale/wrong owner token can never reach free(). Removal and the
// caller's free form one ownership transfer.
class AllocationRegistry {
public:
    bool add(void* base, std::size_t size, std::uint64_t* token) {
        if (!base || size == 0 || !token) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        const std::uint64_t next = next_token_.fetch_add(1, std::memory_order_relaxed);
        if (next == 0) return false;
        const auto inserted = allocations_.emplace(base, Entry{size, next}).second;
        if (inserted) *token = next;
        return inserted;
    }

    bool take(void* base, std::size_t presented_size, std::uint64_t token,
              std::size_t* registered_size = nullptr) {
        if (!base || presented_size == 0 || token == 0) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = allocations_.find(base);
        if (it == allocations_.end() || it->second.size != presented_size ||
            it->second.token != token) return false;
        if (registered_size) *registered_size = it->second.size;
        allocations_.erase(it);
        return true;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return allocations_.size();
    }

private:
    struct Entry {
        std::size_t size;
        std::uint64_t token;
    };

    mutable std::mutex mutex_;
    std::unordered_map<void*, Entry> allocations_;
    std::atomic<std::uint64_t> next_token_{1};
};

}  // namespace spk::jni

#endif  // SPEKTRA_JNI_SAFETY_H

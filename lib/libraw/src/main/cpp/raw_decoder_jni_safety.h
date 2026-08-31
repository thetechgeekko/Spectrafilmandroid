/*
 * Spektrafilm Android -- allocation-free JNI range/copy safety helpers.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
#ifndef SFRAW_RAW_DECODER_JNI_SAFETY_H
#define SFRAW_RAW_DECODER_JNI_SAFETY_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sfraw::jni {

using CancellationCheck = bool (*)(void*);

// The Kotlin facade passes a direct slice whose complete capacity is the
// caller's logical position..limit window. Reject non-canonical JNI callers so
// native code can never silently read bytes outside that window.
inline bool checkedEncodedInputWindow(
        std::int64_t capacity, std::int64_t position, std::int64_t limit,
        std::int64_t length, std::int64_t maximumBytes, std::uintptr_t base,
        std::uintptr_t* address) noexcept {
    if (!address || base == 0U || capacity <= 0 || position < 0 ||
        limit < position || limit > capacity || length <= 0 ||
        maximumBytes <= 0) {
        return false;
    }
    if (position != 0 || limit != capacity || length != capacity ||
        length > maximumBytes) {
        return false;
    }
    *address = base;
    return true;
}

inline bool copyBytesCancellable(
        void* destination, const void* source, std::size_t byteCount,
        CancellationCheck cancellation, void* cancellationContext,
        std::size_t chunkSize = 1024U * 1024U) noexcept {
    if (!destination || !source || byteCount == 0U || chunkSize == 0U) {
        return false;
    }
    auto* out = static_cast<std::uint8_t*>(destination);
    const auto* in = static_cast<const std::uint8_t*>(source);
    std::size_t offset = 0U;
    while (offset < byteCount) {
        if (cancellation && cancellation(cancellationContext)) return false;
        const std::size_t remaining = byteCount - offset;
        const std::size_t copied = remaining < chunkSize ? remaining : chunkSize;
        std::memcpy(out + offset, in + offset, copied);
        offset += copied;
    }
    return !cancellation || !cancellation(cancellationContext);
}

}  // namespace sfraw::jni

#endif  // SFRAW_RAW_DECODER_JNI_SAFETY_H

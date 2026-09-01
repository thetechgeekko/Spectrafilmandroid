/*
 * Spektrafilm Android -- allocation-free JNI input-range safety helper.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
#ifndef SFRAW_RAW_DECODER_JNI_SAFETY_H
#define SFRAW_RAW_DECODER_JNI_SAFETY_H

#include <cstdint>

namespace sfraw::jni {

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

}  // namespace sfraw::jni

#endif  // SFRAW_RAW_DECODER_JNI_SAFETY_H

#ifndef SPECTRAFILM_PNG_WRITER_JNI_BOUNDARY_H
#define SPECTRAFILM_PNG_WRITER_JNI_BOUNDARY_H

#include <cstdint>
#include <exception>
#include <new>

namespace spectrafilm::pngjni {

enum class NativeExceptionKind {
    None,
    OutOfMemory,
    Standard,
    Unknown,
};

enum class BufferWindowError {
    None,
    Malformed,
    TooSmall,
};

struct BufferWindow {
    uint64_t offset = 0;
    uint64_t length = 0;
};

inline BufferWindowError validateBufferWindow(int64_t position, int64_t limit,
                                              int64_t javaCapacity,
                                              int64_t nativeCapacity,
                                              uint64_t requiredBytes,
                                              BufferWindow& window) noexcept {
    window = {};
    if (position < 0 || limit < position || javaCapacity < limit ||
        nativeCapacity < 0 || javaCapacity != nativeCapacity) {
        return BufferWindowError::Malformed;
    }
    const uint64_t remaining = static_cast<uint64_t>(limit - position);
    if (requiredBytes > remaining) return BufferWindowError::TooSmall;
    window.offset = static_cast<uint64_t>(position);
    window.length = requiredBytes;
    return BufferWindowError::None;
}

inline const char* stableMessage(BufferWindowError error) noexcept {
    switch (error) {
        case BufferWindowError::Malformed:
            return "malformed direct ByteBuffer position, limit, or capacity";
        case BufferWindowError::TooSmall:
            return "direct ByteBuffer logical window is too small for packed RGB pixels";
        case BufferWindowError::None:
            return "";
    }
    return "malformed direct ByteBuffer position, limit, or capacity";
}

inline const char* stableMessage(NativeExceptionKind kind) noexcept {
    switch (kind) {
        case NativeExceptionKind::OutOfMemory:
            return "PNG write failed: out of memory";
        case NativeExceptionKind::Standard:
            return "PNG write failed: native exception";
        case NativeExceptionKind::Unknown:
            return "PNG write failed: unknown native exception";
        case NativeExceptionKind::None:
            return "";
    }
    return "PNG write failed: unknown native exception";
}

template <typename Result, typename Function>
Result containNativeExceptions(Function&& function,
                               NativeExceptionKind& kind) noexcept {
    kind = NativeExceptionKind::None;
    try {
        return function();
    } catch (const std::bad_alloc&) {
        kind = NativeExceptionKind::OutOfMemory;
    } catch (const std::exception&) {
        kind = NativeExceptionKind::Standard;
    } catch (...) {
        kind = NativeExceptionKind::Unknown;
    }
    return Result{};
}

}  // namespace spectrafilm::pngjni

#endif  // SPECTRAFILM_PNG_WRITER_JNI_BOUNDARY_H

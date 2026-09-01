/*
 * Spektrafilm for Android -- hardened lib:libraw JNI bridge.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Uses statically included, dual-offered LibRaw; distribution is governed by the
 * bundled decision record and fail-closed release audit.
 */
#include <jni.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>

#include "native_allocation_registry.h"
#include "raw_decoder.h"
#include "raw_decoder_jni_safety.h"
#include "raw_result_publication.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

#define JNI(ret, name) extern "C" JNIEXPORT ret JNICALL \
    Java_com_spectrafilm_libraw_RawDecoder_##name

namespace {

constexpr jlong kMaxEncodedInputBytes = 64LL * 1024LL * 1024LL;
constexpr jint kNativeReleased = 0;
constexpr jint kNativeUnknownToken = 1;
constexpr jint kNativeMismatch = 2;

using CancellationLease =
    std::shared_ptr<sfraw::NativeCancellationRegistry::Flag>;

class ByteArrayElements final {
 public:
    ByteArrayElements(JNIEnv* env, jbyteArray array)
        : env_(env), array_(array), data_(env->GetByteArrayElements(array, nullptr)) {}

    ~ByteArrayElements() {
        if (data_ != nullptr) env_->ReleaseByteArrayElements(array_, data_, JNI_ABORT);
    }

    ByteArrayElements(const ByteArrayElements&) = delete;
    ByteArrayElements& operator=(const ByteArrayElements&) = delete;

    jbyte* get() const { return data_; }

 private:
    JNIEnv* env_;
    jbyteArray array_;
    jbyte* data_;
};

void throwRawDecodeException(JNIEnv* env, const char* message, jint status,
                             jint librawCode) noexcept {
    if (env->ExceptionCheck()) env->ExceptionClear();
    const char* stableMessage = message != nullptr ? message : "RAW decode failed";
    jclass exceptionClass =
        env->FindClass("com/spectrafilm/libraw/RawDecodeException");
    if (exceptionClass != nullptr) {
        jmethodID constructor = env->GetMethodID(
            exceptionClass, "<init>", "(Ljava/lang/String;II)V");
        if (constructor != nullptr) {
            jstring javaMessage = env->NewStringUTF(stableMessage);
            if (javaMessage != nullptr) {
                jobject exception = env->NewObject(
                    exceptionClass, constructor, javaMessage, status, librawCode);
                if (exception != nullptr) {
                    env->Throw(static_cast<jthrowable>(exception));
                    return;
                }
            }
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    jclass fallback = env->FindClass("java/lang/RuntimeException");
    if (fallback != nullptr) env->ThrowNew(fallback, stableMessage);
}

void throwDecodeResult(JNIEnv* env,
                       const spectrafilm::DecodeResult& result) noexcept {
    throwRawDecodeException(
        env,
        result.error.empty() ? "RAW decode failed" : result.error.c_str(),
        static_cast<jint>(result.status),
        static_cast<jint>(result.librawCode));
}

void throwCancelled(JNIEnv* env,
                    const char* message = "RAW decode cancelled") noexcept {
    throwRawDecodeException(env, message, spectrafilm::SFRAW_ERR_CANCELLED, 0);
}

void throwCaught(JNIEnv* env, bool noMemory) noexcept {
    throwRawDecodeException(
        env,
        noMemory ? "RAW JNI bridge out of memory" : "RAW JNI bridge failure",
        noMemory ? spectrafilm::SFRAW_ERR_NO_MEMORY
                 : spectrafilm::SFRAW_ERR_UNKNOWN,
        0);
}

CancellationLease acquireCancellation(JNIEnv* env, jlong token) {
    if (token == 0) return nullptr;
    if (token < 0) {
        throwCancelled(env, "invalid RAW cancellation token");
        return nullptr;
    }
    CancellationLease lease = sfraw::nativeCancellationRegistry().acquire(
        static_cast<std::uint64_t>(token));
    if (lease == nullptr) {
        throwCancelled(env, "RAW cancellation token is closed");
    }
    return lease;
}

bool isCancelled(const CancellationLease& lease) {
    return lease != nullptr && lease->load(std::memory_order_acquire);
}

spectrafilm::DecodeOptions readOptions(
    jint wbMode, jdouble temperatureK, jdouble tint, jboolean halfSize,
    jint maxLongEdge, const CancellationLease& cancellation) {
    spectrafilm::DecodeOptions options;
    switch (wbMode) {
        case 0: options.whiteBalance = spectrafilm::WhiteBalanceMode::AsShot; break;
        case 1: options.whiteBalance = spectrafilm::WhiteBalanceMode::Daylight; break;
        case 2: options.whiteBalance = spectrafilm::WhiteBalanceMode::Tungsten; break;
        case 3: options.whiteBalance = spectrafilm::WhiteBalanceMode::Custom; break;
        default: options.whiteBalance = spectrafilm::WhiteBalanceMode::AsShot; break;
    }
    options.temperatureK = temperatureK;
    options.tint = tint;
    options.halfSize = halfSize != JNI_FALSE;
    options.maxLongEdge = maxLongEdge > 0 ? maxLongEdge : 0;
    options.cancelFlag = cancellation.get();
    return options;
}

bool checkedOutputBytes(const spectrafilm::DecodeResult& result,
                        std::size_t* byteCount) {
    if (result.width <= 0 || result.height <= 0) return false;
    const std::size_t width = static_cast<std::size_t>(result.width);
    const std::size_t height = static_cast<std::size_t>(result.height);
    if (height > std::numeric_limits<std::size_t>::max() / width) return false;
    const std::size_t pixels = width * height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 3U) return false;
    const std::size_t floats = pixels * 3U;
    if (result.rgb.size() != floats) return false;
    if (floats > static_cast<std::size_t>(INT32_MAX) / sizeof(float)) return false;
    *byteCount = floats * sizeof(float);
    return *byteCount != 0U;
}

enum class JavaPublicationFailure {
    None,
    DirectBuffer,
    ResultAbi,
    ColorSpace,
    ResultConstruction,
};

struct JavaPublicationContext {
    JNIEnv* env = nullptr;
    const spectrafilm::DecodeResult* decoded = nullptr;
    jobject javaResult = nullptr;
    JavaPublicationFailure failure = JavaPublicationFailure::None;
    std::chrono::steady_clock::time_point phaseAt;
    double handoffAdoptMs = 0.0;
    double wrapMs = 0.0;
    double constructMs = 0.0;

    double markPhase() {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double, std::milli>(
            now - phaseAt).count();
        phaseAt = now;
        return elapsed;
    }
};

bool cancellationLeaseRequested(void* opaque) noexcept {
    const auto* cancellation = static_cast<const CancellationLease*>(opaque);
    return cancellation != nullptr && isCancelled(*cancellation);
}

sfraw::jni::PublicationDecision attemptJavaPublication(
        const sfraw::jni::NativePublication& publication, void* opaque) {
    auto& context = *static_cast<JavaPublicationContext*>(opaque);
    context.handoffAdoptMs = context.markPhase();

    jobject owned = context.env->NewDirectByteBuffer(
        publication.base, static_cast<jlong>(publication.capacity));
    if (owned == nullptr) {
        context.failure = JavaPublicationFailure::DirectBuffer;
        return sfraw::jni::PublicationDecision::Failed;
    }
    context.wrapMs = context.markPhase();

    jclass resultClass = context.env->FindClass(
        "com/spectrafilm/libraw/RawDecoder$NativeResult");
    jmethodID constructor = resultClass != nullptr
        ? context.env->GetMethodID(
              resultClass, "<init>",
              "(Ljava/nio/ByteBuffer;IILjava/lang/String;J)V")
        : nullptr;
    if (constructor == nullptr) {
        context.failure = JavaPublicationFailure::ResultAbi;
        return sfraw::jni::PublicationDecision::Failed;
    }

    jstring colorSpace =
        context.env->NewStringUTF(context.decoded->colorSpace.c_str());
    if (colorSpace == nullptr) {
        context.failure = JavaPublicationFailure::ColorSpace;
        return sfraw::jni::PublicationDecision::Failed;
    }
    context.javaResult = context.env->NewObject(
        resultClass, constructor, owned,
        static_cast<jint>(context.decoded->width),
        static_cast<jint>(context.decoded->height), colorSpace,
        static_cast<jlong>(publication.token));
    if (context.javaResult == nullptr) {
        context.failure = JavaPublicationFailure::ResultConstruction;
        return sfraw::jni::PublicationDecision::Failed;
    }
    context.constructMs = context.markPhase();
    return sfraw::jni::PublicationDecision::Published;
}

void abortJavaPublication(void* opaque) noexcept {
    auto& context = *static_cast<JavaPublicationContext*>(opaque);
    if (context.javaResult != nullptr) {
        context.env->DeleteLocalRef(context.javaResult);
        context.javaResult = nullptr;
    }
}

jobject toJavaResult(JNIEnv* env, spectrafilm::DecodeResult& result,
                     CancellationLease& cancellation) {
    const auto jniStartedAt = std::chrono::steady_clock::now();
    if (!result.ok) {
        throwDecodeResult(env, result);
        return nullptr;
    }

    std::size_t byteCount = 0U;
    if (!checkedOutputBytes(result, &byteCount)) {
        throwRawDecodeException(env, "invalid RAW output geometry",
                                spectrafilm::SFRAW_ERR_FORMAT, 0);
        return nullptr;
    }

    // DecodeResult already owns malloc-compatible storage. Transfer that exact
    // allocation into the registry: no second full-frame allocation, zero-fill,
    // or memcpy is needed before NewDirectByteBuffer publishes it to Kotlin.
    // The production publication seam owns every release/adopt/failure/cancel
    // transition so no JNI branch can strand or double-free the allocation.
    JavaPublicationContext publicationContext;
    publicationContext.env = env;
    publicationContext.decoded = &result;
    publicationContext.phaseAt = jniStartedAt;
    const auto publication = sfraw::jni::publishDecodedAllocation(
        result.rgb, byteCount, sfraw::nativeAllocationRegistry(),
        cancellationLeaseRequested, &cancellation,
        attemptJavaPublication, &publicationContext,
        abortJavaPublication);

    if (publication.decision == sfraw::jni::PublicationDecision::Cancelled) {
        throwCancelled(env);
        return nullptr;
    }
    if (publication.decision != sfraw::jni::PublicationDecision::Published) {
        switch (publicationContext.failure) {
            case JavaPublicationFailure::DirectBuffer:
                throwRawDecodeException(env, "failed to wrap native RAW output",
                                        spectrafilm::SFRAW_ERR_NO_MEMORY, 0);
                break;
            case JavaPublicationFailure::ResultAbi:
                throwRawDecodeException(env, "RAW NativeResult ABI mismatch",
                                        spectrafilm::SFRAW_ERR_FORMAT, 0);
                break;
            case JavaPublicationFailure::ColorSpace:
                throwRawDecodeException(
                    env, "failed to create RAW color-space metadata",
                    spectrafilm::SFRAW_ERR_NO_MEMORY, 0);
                break;
            case JavaPublicationFailure::ResultConstruction:
                // Replace VM-specific construction failures (most commonly OOM)
                // with the stable typed contract used by every JNI failure path.
                throwRawDecodeException(env, "failed to construct RAW result",
                                        spectrafilm::SFRAW_ERR_NO_MEMORY, 0);
                break;
            case JavaPublicationFailure::None:
                throwRawDecodeException(env, "missing native RAW output",
                                        spectrafilm::SFRAW_ERR_FORMAT, 0);
                break;
        }
        return nullptr;
    }
#ifdef __ANDROID__
    __android_log_print(
        ANDROID_LOG_INFO, "sfraw",
        "jni result ms: handoff_adopt=%.3f wrap=%.3f "
        "construct=%.3f total=%.3f bytes=%zu",
        publicationContext.handoffAdoptMs, publicationContext.wrapMs,
        publicationContext.constructMs,
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - jniStartedAt).count(),
        byteCount);
#endif
    return publicationContext.javaResult;
}

bool validateDirectInput(JNIEnv* env, jobject buffer, jint length,
                         void** address) {
    if (buffer == nullptr) {
        throwRawDecodeException(env, "null RAW ByteBuffer",
                                spectrafilm::SFRAW_ERR_INPUT, 0);
        return false;
    }
    const jlong capacity = env->GetDirectBufferCapacity(buffer);
    void* directAddress = env->GetDirectBufferAddress(buffer);
    if (capacity < 0 || directAddress == nullptr) {
        throwRawDecodeException(env, "expected a direct RAW ByteBuffer",
                                spectrafilm::SFRAW_ERR_INPUT, 0);
        return false;
    }
    jclass bufferClass = env->GetObjectClass(buffer);
    if (bufferClass == nullptr) {
        throwRawDecodeException(env, "cannot inspect RAW ByteBuffer range",
                                spectrafilm::SFRAW_ERR_INPUT, 0);
        return false;
    }
    jmethodID positionMethod = env->GetMethodID(bufferClass, "position", "()I");
    jmethodID limitMethod = env->GetMethodID(bufferClass, "limit", "()I");
    env->DeleteLocalRef(bufferClass);
    if (positionMethod == nullptr || limitMethod == nullptr) {
        throwRawDecodeException(env, "cannot inspect RAW ByteBuffer range",
                                spectrafilm::SFRAW_ERR_INPUT, 0);
        return false;
    }
    const jint position = env->CallIntMethod(buffer, positionMethod);
    if (env->ExceptionCheck()) {
        throwRawDecodeException(env, "cannot inspect RAW ByteBuffer position",
                                spectrafilm::SFRAW_ERR_INPUT, 0);
        return false;
    }
    const jint limit = env->CallIntMethod(buffer, limitMethod);
    if (env->ExceptionCheck()) {
        throwRawDecodeException(env, "cannot inspect RAW ByteBuffer limit",
                                spectrafilm::SFRAW_ERR_INPUT, 0);
        return false;
    }
    std::uintptr_t resolved = 0U;
    if (!sfraw::jni::checkedEncodedInputWindow(
            capacity, position, limit, length, kMaxEncodedInputBytes,
            reinterpret_cast<std::uintptr_t>(directAddress), &resolved)) {
        throwRawDecodeException(env, "invalid RAW ByteBuffer logical range",
                                spectrafilm::SFRAW_ERR_INPUT, 0);
        return false;
    }
    *address = reinterpret_cast<void*>(resolved);
    return true;
}

}  // namespace

JNI(jint, nativeRelease)(JNIEnv* env, jobject, jlong token, jobject buffer) {
    try {
        if (token <= 0 || buffer == nullptr) return kNativeMismatch;
        void* address = env->GetDirectBufferAddress(buffer);
        const jlong capacity = env->GetDirectBufferCapacity(buffer);
        if (address == nullptr || capacity <= 0 ||
            static_cast<std::uint64_t>(capacity) >
                std::numeric_limits<std::size_t>::max() ||
            (reinterpret_cast<std::uintptr_t>(address) % alignof(float)) != 0U ||
            (capacity % static_cast<jlong>(sizeof(float))) != 0) {
            return kNativeMismatch;
        }
        switch (sfraw::nativeAllocationRegistry().release(
            static_cast<std::uint64_t>(token), address,
            static_cast<std::size_t>(capacity))) {
            case sfraw::ReleaseResult::Released: return kNativeReleased;
            case sfraw::ReleaseResult::UnknownToken: return kNativeUnknownToken;
            case sfraw::ReleaseResult::Mismatch: return kNativeMismatch;
        }
    } catch (...) {
        return kNativeMismatch;
    }
    return kNativeMismatch;
}

JNI(jlong, nativeCreateCancellation)(JNIEnv* env, jobject) {
    try {
        return static_cast<jlong>(sfraw::nativeCancellationRegistry().create());
    } catch (const std::bad_alloc&) {
        throwCaught(env, true);
    } catch (const std::exception&) {
        throwCaught(env, false);
    } catch (...) {
        throwCaught(env, false);
    }
    return 0;
}

JNI(jboolean, nativeCancelCancellation)(JNIEnv*, jobject, jlong token) {
    try {
        return token > 0 && sfraw::nativeCancellationRegistry().cancel(
                                static_cast<std::uint64_t>(token))
            ? JNI_TRUE
            : JNI_FALSE;
    } catch (...) {
        return JNI_FALSE;
    }
}

JNI(jboolean, nativeReleaseCancellation)(JNIEnv*, jobject, jlong token) {
    try {
        return token > 0 && sfraw::nativeCancellationRegistry().release(
                                static_cast<std::uint64_t>(token))
            ? JNI_TRUE
            : JNI_FALSE;
    } catch (...) {
        return JNI_FALSE;
    }
}

JNI(jobject, nativeDecodeBytes)(JNIEnv* env, jobject, jbyteArray bytes,
                                jint wbMode, jdouble temperatureK, jdouble tint,
                                jboolean halfSize, jint maxLongEdge,
                                jlong cancellationToken) {
    try {
        if (bytes == nullptr) {
            throwRawDecodeException(env, "null RAW byte array",
                                    spectrafilm::SFRAW_ERR_INPUT, 0);
            return nullptr;
        }
        const jsize length = env->GetArrayLength(bytes);
        if (length <= 0 || static_cast<jlong>(length) > kMaxEncodedInputBytes) {
            throwRawDecodeException(env, "invalid RAW byte-array length",
                                    spectrafilm::SFRAW_ERR_INPUT, 0);
            return nullptr;
        }
        CancellationLease cancellation =
            acquireCancellation(env, cancellationToken);
        if (cancellationToken != 0 && cancellation == nullptr) return nullptr;
        if (isCancelled(cancellation)) {
            throwCancelled(env);
            return nullptr;
        }
        ByteArrayElements input(env, bytes);
        if (input.get() == nullptr) {
            throwRawDecodeException(env, "failed to pin RAW byte array",
                                    spectrafilm::SFRAW_ERR_NO_MEMORY, 0);
            return nullptr;
        }
        if (isCancelled(cancellation)) {
            throwCancelled(env);
            return nullptr;
        }
        spectrafilm::DecodeResult result = spectrafilm::decodeFromBuffer(
            reinterpret_cast<const std::uint8_t*>(input.get()),
            static_cast<std::size_t>(length),
            readOptions(wbMode, temperatureK, tint, halfSize, maxLongEdge,
                        cancellation));
        if (isCancelled(cancellation) && result.ok) {
            throwCancelled(env);
            return nullptr;
        }
        return toJavaResult(env, result, cancellation);
    } catch (const std::bad_alloc&) {
        throwCaught(env, true);
    } catch (const std::exception&) {
        throwCaught(env, false);
    } catch (...) {
        throwCaught(env, false);
    }
    return nullptr;
}

JNI(jobject, nativeDecodeBuffer)(JNIEnv* env, jobject, jobject directBuffer,
                                 jint length, jint wbMode,
                                 jdouble temperatureK, jdouble tint,
                                 jboolean halfSize, jint maxLongEdge,
                                 jlong cancellationToken) {
    try {
        void* address = nullptr;
        if (!validateDirectInput(env, directBuffer, length, &address)) return nullptr;
        CancellationLease cancellation =
            acquireCancellation(env, cancellationToken);
        if (cancellationToken != 0 && cancellation == nullptr) return nullptr;
        if (isCancelled(cancellation)) {
            throwCancelled(env);
            return nullptr;
        }
        spectrafilm::DecodeResult result = spectrafilm::decodeFromBuffer(
            reinterpret_cast<const std::uint8_t*>(address),
            static_cast<std::size_t>(length),
            readOptions(wbMode, temperatureK, tint, halfSize, maxLongEdge,
                        cancellation));
        if (isCancelled(cancellation) && result.ok) {
            throwCancelled(env);
            return nullptr;
        }
        return toJavaResult(env, result, cancellation);
    } catch (const std::bad_alloc&) {
        throwCaught(env, true);
    } catch (const std::exception&) {
        throwCaught(env, false);
    } catch (...) {
        throwCaught(env, false);
    }
    return nullptr;
}

JNI(jobject, nativeDecodeFd)(JNIEnv* env, jobject, jint fd, jint wbMode,
                             jdouble temperatureK, jdouble tint,
                             jboolean halfSize, jint maxLongEdge,
                             jlong cancellationToken) {
    try {
        CancellationLease cancellation =
            acquireCancellation(env, cancellationToken);
        if (cancellationToken != 0 && cancellation == nullptr) return nullptr;
        if (isCancelled(cancellation)) {
            throwCancelled(env);
            return nullptr;
        }
        spectrafilm::DecodeResult result = spectrafilm::decodeFromFd(
            fd, readOptions(wbMode, temperatureK, tint, halfSize, maxLongEdge,
                            cancellation));
        if (isCancelled(cancellation) && result.ok) {
            throwCancelled(env);
            return nullptr;
        }
        return toJavaResult(env, result, cancellation);
    } catch (const std::bad_alloc&) {
        throwCaught(env, true);
    } catch (const std::exception&) {
        throwCaught(env, false);
    } catch (...) {
        throwCaught(env, false);
    }
    return nullptr;
}

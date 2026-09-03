/*
 * Spektrafilm for Android — lib:pngwriter JNI bridge.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
#include <jni.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "png_writer.h"
#include "png_writer_jni_boundary.h"

#define JNI_PNG(ret, name) extern "C" JNIEXPORT ret JNICALL \
    Java_com_spectrafilm_pngwriter_PngWriter_##name

namespace {

template <typename T>
class LocalRef final {
public:
    LocalRef(JNIEnv* env, T ref) noexcept : env_(env), ref_(ref) {}
    LocalRef(const LocalRef&) = delete;
    LocalRef& operator=(const LocalRef&) = delete;
    ~LocalRef() { if (ref_ != nullptr) env_->DeleteLocalRef(ref_); }
    T get() const noexcept { return ref_; }

private:
    JNIEnv* env_;
    T ref_;
};

class UtfChars final {
public:
    UtfChars(JNIEnv* env, jstring value) noexcept
        : env_(env), value_(value), chars_(value == nullptr ? nullptr
                                                           : env->GetStringUTFChars(value, nullptr)) {}
    UtfChars(const UtfChars&) = delete;
    UtfChars& operator=(const UtfChars&) = delete;
    ~UtfChars() { if (chars_ != nullptr) env_->ReleaseStringUTFChars(value_, chars_); }
    const char* get() const noexcept { return chars_; }

private:
    JNIEnv* env_;
    jstring value_;
    const char* chars_;
};

class ShortArrayElements final {
public:
    ShortArrayElements(JNIEnv* env, jshortArray array) noexcept
        : env_(env), array_(array), values_(env->GetShortArrayElements(array, nullptr)) {}
    ShortArrayElements(const ShortArrayElements&) = delete;
    ShortArrayElements& operator=(const ShortArrayElements&) = delete;
    ~ShortArrayElements() {
        if (values_ != nullptr) env_->ReleaseShortArrayElements(array_, values_, JNI_ABORT);
    }
    jshort* get() const noexcept { return values_; }

private:
    JNIEnv* env_;
    jshortArray array_;
    jshort* values_;
};

void throwJava(JNIEnv* env, const char* className, const char* message) noexcept {
    if (env->ExceptionCheck()) return;
    LocalRef<jclass> type(env, env->FindClass(className));
    if (type.get() != nullptr) env->ThrowNew(type.get(), message);
}

void throwIae(JNIEnv* env, const char* message) noexcept {
    throwJava(env, "java/lang/IllegalArgumentException", message);
}

void throwIse(JNIEnv* env, const char* message) noexcept {
    throwJava(env, "java/lang/IllegalStateException", message);
}

void throwCancelled(JNIEnv* env) noexcept {
    throwJava(env, "java/util/concurrent/CancellationException", "PNG write cancelled");
}

template <typename Function>
jlong jniBoundary(JNIEnv* env, Function&& function) noexcept {
    spectrafilm::pngjni::NativeExceptionKind kind;
    const jlong result = spectrafilm::pngjni::containNativeExceptions<jlong>(
        function, kind);
    if (kind != spectrafilm::pngjni::NativeExceptionKind::None)
        throwIse(env, spectrafilm::pngjni::stableMessage(kind));
    return result;
}

bool checkedMul(uint64_t a, uint64_t b, uint64_t& out) noexcept {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) return false;
    out = a * b;
    return true;
}

bool requiredBytes(jint width, jint height, uint64_t bytesPerSample,
                   uint64_t& out) noexcept {
    if (width <= 0 || height <= 0) return false;
    uint64_t rowSamples = 0;
    uint64_t rowBytes = 0;
    return checkedMul(static_cast<uint64_t>(width), 3u, rowSamples) &&
           checkedMul(rowSamples, bytesPerSample, rowBytes) &&
           checkedMul(rowBytes, static_cast<uint64_t>(height), out) &&
           out <= static_cast<uint64_t>(std::numeric_limits<jlong>::max());
}

bool stringValue(JNIEnv* env, jstring value, std::string& out) {
    if (value == nullptr) {
        out.clear();
        return true;
    }
    UtfChars chars(env, value);
    if (chars.get() == nullptr) return false;
    out.assign(chars.get());
    return !env->ExceptionCheck();
}

bool readIcc(JNIEnv* env, jbyteArray icc, spectrafilm::PngMetadata& meta) {
    if (icc == nullptr) return true;
    const jsize length = env->GetArrayLength(icc);
    if (env->ExceptionCheck()) return false;
    if (length <= 0) return true;
    meta.iccProfile.resize(static_cast<size_t>(length));
    env->GetByteArrayRegion(icc, 0, length,
                            reinterpret_cast<jbyte*>(meta.iccProfile.data()));
    return !env->ExceptionCheck();
}

bool buildMeta(JNIEnv* env, jstring software, jbyteArray icc,
               spectrafilm::PngMetadata& meta) {
    if (software != nullptr && !stringValue(env, software, meta.software)) return false;
    return readIcc(env, icc, meta);
}

class JavaCancellation final {
public:
    bool initialise(JNIEnv* env, jobject signal) noexcept {
        env_ = env;
        signal_ = signal;
        if (signal == nullptr) return true;
        LocalRef<jclass> type(env, env->GetObjectClass(signal));
        if (type.get() == nullptr) return false;
        get_ = env->GetMethodID(type.get(), "get", "()Z");
        return get_ != nullptr && !env->ExceptionCheck();
    }

    bool present() const noexcept { return signal_ != nullptr; }
    bool callbackFailed() const noexcept { return callbackFailed_; }
    bool cancelledNow() noexcept { return present() && poll(this); }

    static bool poll(void* opaque) noexcept {
        auto* self = static_cast<JavaCancellation*>(opaque);
        const jboolean cancelled = self->env_->CallBooleanMethod(self->signal_, self->get_);
        if (self->env_->ExceptionCheck()) {
            self->callbackFailed_ = true;
            return true;
        }
        return cancelled == JNI_TRUE;
    }

private:
    JNIEnv* env_ = nullptr;
    jobject signal_ = nullptr;
    jmethodID get_ = nullptr;
    bool callbackFailed_ = false;
};

bool validateDirectBuffer(JNIEnv* env, jobject buffer, jint width, jint height,
                          uint64_t bytesPerSample, size_t alignment,
                          void*& address) noexcept {
    address = nullptr;
    uint64_t needed = 0;
    if (!requiredBytes(width, height, bytesPerSample, needed)) {
        throwIae(env, "PNG dimensions or pixel byte count overflow");
        return false;
    }
    if (buffer == nullptr) {
        throwIae(env, "pixel buffer must not be null");
        return false;
    }

    void* baseAddress = env->GetDirectBufferAddress(buffer);
    const jlong nativeCapacity = env->GetDirectBufferCapacity(buffer);
    if (baseAddress == nullptr || nativeCapacity < 0) {
        throwIae(env, "pixel buffer must be a direct ByteBuffer");
        return false;
    }

    LocalRef<jclass> type(env, env->GetObjectClass(buffer));
    if (type.get() == nullptr) return false;
    const jmethodID positionMethod = env->GetMethodID(type.get(), "position", "()I");
    const jmethodID limitMethod = env->GetMethodID(type.get(), "limit", "()I");
    const jmethodID capacityMethod = env->GetMethodID(type.get(), "capacity", "()I");
    if (positionMethod == nullptr || limitMethod == nullptr || capacityMethod == nullptr ||
        env->ExceptionCheck()) return false;

    const jint position = env->CallIntMethod(buffer, positionMethod);
    if (env->ExceptionCheck()) return false;
    const jint limit = env->CallIntMethod(buffer, limitMethod);
    if (env->ExceptionCheck()) return false;
    const jint javaCapacity = env->CallIntMethod(buffer, capacityMethod);
    if (env->ExceptionCheck()) return false;

    spectrafilm::pngjni::BufferWindow window;
    const auto windowError = spectrafilm::pngjni::validateBufferWindow(
        position, limit, javaCapacity, nativeCapacity, needed, window);
    if (windowError != spectrafilm::pngjni::BufferWindowError::None) {
        throwIae(env, spectrafilm::pngjni::stableMessage(windowError));
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(baseAddress);
    if (window.offset > std::numeric_limits<uintptr_t>::max() - base) {
        throwIae(env, "direct ByteBuffer address overflow");
        return false;
    }
    const uintptr_t selected = base + static_cast<uintptr_t>(window.offset);
    if (selected % alignment != 0u) {
        throwIae(env, "direct ByteBuffer address is not sample-aligned");
        return false;
    }
    address = reinterpret_cast<void*>(selected);
    return true;
}

bool outputPathValue(JNIEnv* env, jstring value, std::string& path) {
    if (value == nullptr) {
        throwIae(env, "output path must not be null");
        return false;
    }
    if (!stringValue(env, value, path)) return false;
    if (path.empty()) {
        throwIae(env, "output path must not be empty");
        return false;
    }
    if (path.find('\0') != std::string::npos) {
        throwIae(env, "output path must not contain NUL");
        return false;
    }
    return true;
}

void throwResult(JNIEnv* env, const spectrafilm::PngWriteResult& result) {
    if (result.cancelled) {
        throwCancelled(env);
        return;
    }
    const std::string message = result.error.empty()
        ? "PNG write failed"
        : "PNG write failed: " + result.error;
    throwIse(env, message.c_str());
}

}  // namespace

JNI_PNG(jlong, nativeWriteBuffer)(JNIEnv* env, jobject /*thiz*/, jobject directBuffer,
                                  jint width, jint height,
                                  jstring software, jbyteArray iccBytes,
                                  jstring outPath, jobject cancellationSignal) {
    return jniBoundary(env, [&]() -> jlong {
        void* address = nullptr;
        if (!validateDirectBuffer(env, directBuffer, width, height, 2u,
                                  alignof(uint16_t), address)) return 0;

        spectrafilm::PngMetadata meta;
        std::string path;
        if (!outputPathValue(env, outPath, path)) return 0;

        JavaCancellation javaCancellation;
        if (!javaCancellation.initialise(env, cancellationSignal)) return 0;
        if (javaCancellation.cancelledNow()) {
            if (!javaCancellation.callbackFailed()) throwCancelled(env);
            return 0;
        }
        if (!buildMeta(env, software, iccBytes, meta)) return 0;
        spectrafilm::PngCancellation cancellation{&javaCancellation, JavaCancellation::poll};
        const spectrafilm::PngCancellation* cancellationPtr =
            javaCancellation.present() ? &cancellation : nullptr;
        const spectrafilm::PngWriteResult result = spectrafilm::writePng16ToFile(
            static_cast<const uint16_t*>(address), width, height, meta, path,
            cancellationPtr);
        if (javaCancellation.callbackFailed() || env->ExceptionCheck()) return 0;
        if (!result.ok) { throwResult(env, result); return 0; }
        return static_cast<jlong>(result.bytesWritten);
    });
}

// Float entry: the engine's own display-referred float buffer, quantized to uint16
// one row at a time inside the writer. The caller used to do that in a JVM loop over
// every sample and hand over a second 75 MB buffer; at 12.5 MP this removes both
// (#175). The arithmetic is deliberately identical to the loop it replaces --
// clamp to [0,1], v*65535+0.5, truncate, NaN to 0 -- so the pixels do not move.
JNI_PNG(jlong, nativeWriteFloatBuffer)(JNIEnv* env, jobject /*thiz*/, jobject directBuffer,
                                       jint width, jint height,
                                       jstring software, jbyteArray iccBytes,
                                       jstring outPath, jobject cancellationSignal) {
    return jniBoundary(env, [&]() -> jlong {
        void* address = nullptr;
        if (!validateDirectBuffer(env, directBuffer, width, height, 4u,
                                  alignof(float), address)) return 0;

        spectrafilm::PngMetadata meta;
        std::string path;
        if (!outputPathValue(env, outPath, path)) return 0;

        JavaCancellation javaCancellation;
        if (!javaCancellation.initialise(env, cancellationSignal)) return 0;
        if (javaCancellation.cancelledNow()) {
            if (!javaCancellation.callbackFailed()) throwCancelled(env);
            return 0;
        }
        if (!buildMeta(env, software, iccBytes, meta)) return 0;
        spectrafilm::PngCancellation cancellation{&javaCancellation, JavaCancellation::poll};
        const spectrafilm::PngCancellation* cancellationPtr =
            javaCancellation.present() ? &cancellation : nullptr;
        const spectrafilm::PngWriteResult result = spectrafilm::writePngFloatToFile(
            static_cast<const float*>(address), width, height, meta, path,
            cancellationPtr);
        if (javaCancellation.callbackFailed() || env->ExceptionCheck()) return 0;
        if (!result.ok) { throwResult(env, result); return 0; }
        return static_cast<jlong>(result.bytesWritten);
    });
}

JNI_PNG(jlong, nativeWriteShorts)(JNIEnv* env, jobject /*thiz*/, jshortArray rgb16,
                                  jint width, jint height,
                                  jstring software, jbyteArray iccBytes,
                                  jstring outPath, jobject cancellationSignal) {
    return jniBoundary(env, [&]() -> jlong {
        uint64_t neededBytes = 0;
        if (!requiredBytes(width, height, 2u, neededBytes)) {
            throwIae(env, "PNG dimensions or pixel count overflow");
            return 0;
        }
        if (rgb16 == nullptr) { throwIae(env, "pixel short array must not be null"); return 0; }
        const uint64_t neededSamples = neededBytes / 2u;
        const jsize length = env->GetArrayLength(rgb16);
        if (env->ExceptionCheck()) return 0;
        if (neededSamples > static_cast<uint64_t>(length)) {
            throwIae(env, "pixel short array is too small for packed RGB pixels");
            return 0;
        }
        spectrafilm::PngMetadata meta;
        std::string path;
        if (!outputPathValue(env, outPath, path)) return 0;

        JavaCancellation javaCancellation;
        if (!javaCancellation.initialise(env, cancellationSignal)) return 0;
        if (javaCancellation.cancelledNow()) {
            if (!javaCancellation.callbackFailed()) throwCancelled(env);
            return 0;
        }
        if (!buildMeta(env, software, iccBytes, meta)) return 0;

        ShortArrayElements pixels(env, rgb16);
        if (pixels.get() == nullptr) {
            if (!env->ExceptionCheck()) throwIse(env, "PNG write failed: cannot access pixels");
            return 0;
        }
        spectrafilm::PngCancellation cancellation{&javaCancellation, JavaCancellation::poll};
        const spectrafilm::PngCancellation* cancellationPtr =
            javaCancellation.present() ? &cancellation : nullptr;
        const spectrafilm::PngWriteResult result = spectrafilm::writePng16ToFile(
            reinterpret_cast<const uint16_t*>(pixels.get()), width, height,
            meta, path, cancellationPtr);
        if (javaCancellation.callbackFailed() || env->ExceptionCheck()) return 0;
        if (!result.ok) { throwResult(env, result); return 0; }
        return static_cast<jlong>(result.bytesWritten);
    });
}

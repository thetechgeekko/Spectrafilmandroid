/*
 * Spektrafilm for Android — guarded API-30 AImageDecoder JNI experiment.
 * GPL-3.0-only.
 */
#include "image_input_gate.h"

#include <android/bitmap.h>
#include <android/imagedecoder.h>
#include <jni.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <iterator>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {

#define SFA_REQUIRES_API(level) \
    __attribute__((availability(android, introduced = level)))

constexpr const char* kProbeVersion = "sfaimage.probe.v1";
constexpr const char* kDecodeVersion = "sfaimage.decode.v1";
constexpr std::size_t kSniffBytes = 128;
constexpr int kRgba8888 = ANDROID_BITMAP_FORMAT_RGBA_8888;
constexpr int kRgbaF16 = ANDROID_BITMAP_FORMAT_RGBA_F16;

std::atomic<jlong> g_live_decoders{0};
std::atomic<jlong> g_live_fds{0};
jclass g_fallback_class = nullptr;

void throw_named_cstr(JNIEnv* env, const char* class_name,
                      const char* message) noexcept {
    if (env->ExceptionCheck()) return;
    jclass cls = env->FindClass(class_name);
    if (cls == nullptr) return;
    env->ThrowNew(cls, message);
    env->DeleteLocalRef(cls);
}

void throw_fallback(JNIEnv* env, const char* message) noexcept {
    if (env->ExceptionCheck()) return;
    if (g_fallback_class == nullptr) {
        throw_named_cstr(env, "java/lang/IllegalStateException",
                         "AImageDecoder fallback class was not bootstrapped");
        return;
    }
    env->ThrowNew(g_fallback_class, message);
}

void throw_fallback(JNIEnv* env, const std::string& message) noexcept {
    throw_fallback(env, message.c_str());
}

void throw_cancelled(JNIEnv* env) noexcept {
    throw_named_cstr(env, "java/util/concurrent/CancellationException",
                     "AImageDecoder cancelled");
}

template <typename Function>
jstring guard_jstring(JNIEnv* env, Function&& function) noexcept {
    try {
        return function();
    } catch (const std::bad_alloc&) {
        throw_named_cstr(env, "java/lang/OutOfMemoryError",
                         "AImageDecoder native bookkeeping allocation failed");
    } catch (const std::exception&) {
        throw_fallback(env, "AImageDecoder native bookkeeping failure");
    } catch (...) {
        throw_fallback(env, "AImageDecoder native bookkeeping failure");
    }
    return nullptr;
}

template <typename Function>
void guard_void(JNIEnv* env, Function&& function) noexcept {
    try {
        function();
    } catch (const std::bad_alloc&) {
        throw_named_cstr(env, "java/lang/OutOfMemoryError",
                         "AImageDecoder native bookkeeping allocation failed");
    } catch (const std::exception&) {
        throw_fallback(env, "AImageDecoder native bookkeeping failure");
    } catch (...) {
        throw_fallback(env, "AImageDecoder native bookkeeping failure");
    }
}

std::string lower_ascii(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

bool get_nullable_utf8(JNIEnv* env, jstring value, std::string* out) {
    out->clear();
    if (value == nullptr) return true;
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) return false;
    try {
        out->assign(chars);
    } catch (...) {
        env->ReleaseStringUTFChars(value, chars);
        throw;
    }
    env->ReleaseStringUTFChars(value, chars);
    return true;
}

bool get_declared_mime(JNIEnv* env, jstring value, std::string* out) {
    if (!get_nullable_utf8(env, value, out)) return false;
    if (out->size() > 127U || out->find('\t') != std::string::npos ||
        out->find('\n') != std::string::npos ||
        out->find('\r') != std::string::npos) {
        throw_fallback(env, "declared MIME is too long or contains a control character");
        return false;
    }
    *out = lower_ascii(std::move(*out));
    return true;
}

class OwnedFd final {
  public:
    explicit OwnedFd(int fd = -1) noexcept : fd_(fd) {
        if (fd_ >= 0) g_live_fds.fetch_add(1, std::memory_order_relaxed);
    }
    ~OwnedFd() { reset(); }
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    OwnedFd(OwnedFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    OwnedFd& operator=(OwnedFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }

  private:
    void reset() noexcept {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
            g_live_fds.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    int fd_;
};

OwnedFd reopen_fd(int source_fd, std::string* error) {
    if (source_fd < 0) {
        *error = "negative file descriptor";
        return OwnedFd();
    }
    char proc_path[64] = {};
    const int length = std::snprintf(proc_path, sizeof(proc_path),
                                     "/proc/self/fd/%d", source_fd);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(proc_path)) {
        *error = "file descriptor path overflow";
        return OwnedFd();
    }
    const int reopened = open(proc_path, O_RDONLY | O_CLOEXEC);
    if (reopened < 0) {
        *error = "file descriptor is not independently reopenable/seekable";
        return OwnedFd();
    }
    return OwnedFd(reopened);
}

bool inspect_fd(OwnedFd* fd, std::uint64_t* encoded_size,
                std::array<std::uint8_t, kSniffBytes>* header,
                std::size_t* header_size, std::string* error) {
    const off_t end = lseek(fd->get(), 0, SEEK_END);
    if (end <= 0 || static_cast<std::uint64_t>(end) > sfaimage::kMaxEncodedBytes) {
        *error = end < 0 ? "file descriptor is not seekable"
                         : "encoded input is empty or exceeds 128 MiB";
        return false;
    }
    if (lseek(fd->get(), 0, SEEK_SET) != 0) {
        *error = "file descriptor cannot seek to its encoded origin";
        return false;
    }
    const std::size_t wanted = std::min<std::size_t>(
            header->size(), static_cast<std::size_t>(end));
    const ssize_t count = pread(fd->get(), header->data(), wanted, 0);
    if (count <= 0 || static_cast<std::size_t>(count) != wanted) {
        *error = "encoded header read failed";
        return false;
    }
    *encoded_size = static_cast<std::uint64_t>(end);
    *header_size = wanted;
    return true;
}

struct DirectInput {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

bool inspect_direct_buffer(JNIEnv* env, jobject buffer, DirectInput* input) {
    if (buffer == nullptr) {
        throw_fallback(env, "encoded direct buffer is null");
        return false;
    }
    void* address = env->GetDirectBufferAddress(buffer);
    const jlong capacity = env->GetDirectBufferCapacity(buffer);
    if (address == nullptr || capacity <= 0 ||
        static_cast<std::uint64_t>(capacity) > sfaimage::kMaxEncodedBytes) {
        throw_fallback(env, "encoded buffer must be direct, non-empty and <= 128 MiB");
        return false;
    }
    input->data = static_cast<const std::uint8_t*>(address);
    input->size = static_cast<std::size_t>(capacity);
    return true;
}

class DecoderOwner final {
  public:
    using DeleteFunction = void (*)(AImageDecoder*);
    DecoderOwner(AImageDecoder* decoder, DeleteFunction deleter) noexcept
        : decoder_(decoder), deleter_(deleter) {
        if (decoder_ != nullptr) {
            g_live_decoders.fetch_add(1, std::memory_order_relaxed);
        }
    }
    ~DecoderOwner() {
        if (decoder_ != nullptr) {
            deleter_(decoder_);
            g_live_decoders.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    DecoderOwner(const DecoderOwner&) = delete;
    DecoderOwner& operator=(const DecoderOwner&) = delete;
    AImageDecoder* get() const noexcept { return decoder_; }

  private:
    AImageDecoder* decoder_;
    DeleteFunction deleter_;
};

struct Probe {
    int width = 0;
    int height = 0;
    int default_format = 0;
    int alpha_flags = 0;
    int data_space = 0;
    std::uint64_t encoded_size = 0;
    sfaimage::InputKind kind = sfaimage::InputKind::Jpeg;
    std::string mime;

    std::string wire() const {
        std::ostringstream out;
        out << kProbeVersion << '\t' << width << '\t' << height << '\t'
            << default_format << '\t' << alpha_flags << '\t' << data_space
            << '\t' << encoded_size << '\t' << sfaimage::kind_name(kind)
            << '\t' << mime;
        return out.str();
    }
};

bool finish_probe(JNIEnv* env, AImageDecoder* decoder,
                  sfaimage::InputKind sniffed_kind,
                  std::uint64_t encoded_size,
                  std::string_view declared_mime,
                  bool enforce_declared_mime,
                  bool allow_dng_fallback,
                  Probe* probe) SFA_REQUIRES_API(30) {
    const AImageDecoderHeaderInfo* info = AImageDecoder_getHeaderInfo(decoder);
    if (info == nullptr) {
        throw_fallback(env, "AImageDecoder returned no header information");
        return false;
    }
    const char* raw_mime = AImageDecoderHeaderInfo_getMimeType(info);
    if (raw_mime == nullptr) {
        throw_fallback(env, "AImageDecoder returned no platform MIME");
        return false;
    }
    probe->width = AImageDecoderHeaderInfo_getWidth(info);
    probe->height = AImageDecoderHeaderInfo_getHeight(info);
    probe->default_format = AImageDecoderHeaderInfo_getAndroidBitmapFormat(info);
    probe->alpha_flags = AImageDecoderHeaderInfo_getAlphaFlags(info);
    probe->data_space = AImageDecoderHeaderInfo_getDataSpace(info);
    probe->encoded_size = encoded_size;
    probe->kind = sniffed_kind;
    probe->mime = lower_ascii(raw_mime);

    if (probe->width <= 0 || probe->height <= 0) {
        throw_fallback(env, "AImageDecoder reported invalid dimensions");
        return false;
    }
    if (!sfaimage::mime_matches(sniffed_kind, probe->mime)) {
        throw_fallback(env, "platform MIME disagrees with the encoded header");
        return false;
    }
    if (enforce_declared_mime &&
        !sfaimage::declared_mime_allows(sniffed_kind, declared_mime,
                                       allow_dng_fallback)) {
        throw_fallback(env, "declared MIME/fallback policy rejected the encoded header");
        return false;
    }
    return true;
}

std::string result_message(const char* operation, int result) {
    std::ostringstream out;
    out << operation << " failed with AImageDecoder result " << result;
    return out.str();
}

bool is_cancelled(JNIEnv* env, jobject cancellation) {
    if (cancellation == nullptr) return false;
    jclass cls = env->GetObjectClass(cancellation);
    if (cls == nullptr) return false;
    jmethodID method = env->GetMethodID(cls, "isCancellationRequested", "()Z");
    env->DeleteLocalRef(cls);
    if (method == nullptr) return false;
    const jboolean requested = env->CallBooleanMethod(cancellation, method);
    // Preserve a Java callback failure and stop all further JNI/platform work.
    if (env->ExceptionCheck()) return true;
    return requested == JNI_TRUE;
}

jstring to_jstring(JNIEnv* env, const std::string& value) {
    return env->NewStringUTF(value.c_str());
}

jstring probe_fd_api30(JNIEnv* env, jint source_fd, jstring declared_mime_value,
                       jboolean allow_dng) SFA_REQUIRES_API(30) {
    std::string declared_mime;
    if (!get_declared_mime(env, declared_mime_value, &declared_mime)) return nullptr;
    std::string error;
    OwnedFd fd = reopen_fd(source_fd, &error);
    if (!fd) {
        throw_fallback(env, error);
        return nullptr;
    }
    std::array<std::uint8_t, kSniffBytes> header{};
    std::uint64_t encoded_size = 0;
    std::size_t header_size = 0;
    if (!inspect_fd(&fd, &encoded_size, &header, &header_size, &error)) {
        throw_fallback(env, error);
        return nullptr;
    }
    const auto kind = sfaimage::sniff_input(header.data(), header_size, encoded_size);
    if (!kind) {
        throw_fallback(env, "encoded header is outside the AImageDecoder allowlist");
        return nullptr;
    }
    AImageDecoder* raw_decoder = nullptr;
    const int created = AImageDecoder_createFromFd(fd.get(), &raw_decoder);
    if (created != ANDROID_IMAGE_DECODER_SUCCESS || raw_decoder == nullptr) {
        throw_fallback(env, result_message("createFromFd", created));
        return nullptr;
    }
    // The NDK contract leaves the out-pointer uninitialized on failure. Never
    // inspect or adopt it until both the status and pointer prove success.
    DecoderOwner decoder(raw_decoder, &AImageDecoder_delete);
    Probe probe;
    if (!finish_probe(env, decoder.get(), *kind, encoded_size, declared_mime,
                      true, allow_dng == JNI_TRUE, &probe)) {
        return nullptr;
    }
    return to_jstring(env, probe.wire());
}

jstring probe_buffer_api30(JNIEnv* env, jobject encoded,
                           jstring declared_mime_value,
                           jboolean allow_dng) SFA_REQUIRES_API(30) {
    std::string declared_mime;
    if (!get_declared_mime(env, declared_mime_value, &declared_mime)) return nullptr;
    DirectInput input;
    if (!inspect_direct_buffer(env, encoded, &input)) return nullptr;
    const auto kind = sfaimage::sniff_input(
            input.data, std::min(input.size, kSniffBytes), input.size);
    if (!kind) {
        throw_fallback(env, "encoded header is outside the AImageDecoder allowlist");
        return nullptr;
    }
    AImageDecoder* raw_decoder = nullptr;
    const int created = AImageDecoder_createFromBuffer(
            input.data, input.size, &raw_decoder);
    if (created != ANDROID_IMAGE_DECODER_SUCCESS || raw_decoder == nullptr) {
        throw_fallback(env, result_message("createFromBuffer", created));
        return nullptr;
    }
    DecoderOwner decoder(raw_decoder, &AImageDecoder_delete);
    Probe probe;
    if (!finish_probe(env, decoder.get(), *kind, input.size, declared_mime,
                      true, allow_dng == JNI_TRUE, &probe)) {
        return nullptr;
    }
    return to_jstring(env, probe.wire());
}

bool configure_and_decode(JNIEnv* env, AImageDecoder* decoder,
                          const Probe& probe, jint target_width,
                          jint target_height, jint bitmap_format,
                          jint output_data_space, jboolean set_data_space,
                          jboolean require_unpremultiplied, jobject pixels,
                          jobject cancellation,
                          std::string* response) SFA_REQUIRES_API(30) {
    if (is_cancelled(env, cancellation)) {
        throw_cancelled(env);
        return false;
    }
    if (target_width <= 0 || target_height <= 0 ||
        (bitmap_format != kRgba8888 && bitmap_format != kRgbaF16)) {
        throw_fallback(env, "invalid explicit AImageDecoder output plan");
        return false;
    }
    const bool scaled = target_width != probe.width || target_height != probe.height;
    if (scaled && require_unpremultiplied == JNI_TRUE) {
        throw_fallback(env, "scaled unpremultiplied AImageDecoder output is forbidden");
        return false;
    }
    int result = AImageDecoder_setAndroidBitmapFormat(decoder, bitmap_format);
    if (result != ANDROID_IMAGE_DECODER_SUCCESS) {
        throw_fallback(env, result_message("setAndroidBitmapFormat", result));
        return false;
    }
    if (set_data_space == JNI_TRUE) {
        result = AImageDecoder_setDataSpace(decoder, output_data_space);
        if (result != ANDROID_IMAGE_DECODER_SUCCESS) {
            throw_fallback(env, result_message("setDataSpace", result));
            return false;
        }
    } else if (output_data_space != probe.data_space) {
        throw_fallback(env, "preserved dataspace differs from the probed header");
        return false;
    }
    if (scaled) {
        result = AImageDecoder_setTargetSize(decoder, target_width, target_height);
        if (result != ANDROID_IMAGE_DECODER_SUCCESS) {
            throw_fallback(env, result_message("setTargetSize", result));
            return false;
        }
    }
    if (require_unpremultiplied == JNI_TRUE) {
        result = AImageDecoder_setUnpremultipliedRequired(decoder, true);
        if (result != ANDROID_IMAGE_DECODER_SUCCESS) {
            throw_fallback(env, result_message("setUnpremultipliedRequired", result));
            return false;
        }
    }
    const std::size_t expected_bpp = bitmap_format == kRgba8888 ? 4U : 8U;
    if (static_cast<std::uint64_t>(target_width) >
        std::numeric_limits<std::size_t>::max() / expected_bpp) {
        throw_fallback(env, "AImageDecoder row stride overflow");
        return false;
    }
    const std::size_t expected_stride =
            static_cast<std::size_t>(target_width) * expected_bpp;
    const std::size_t stride = AImageDecoder_getMinimumStride(decoder);
    if (stride != expected_stride ||
        static_cast<std::uint64_t>(target_height) >
            std::numeric_limits<std::size_t>::max() / stride) {
        throw_fallback(env, "AImageDecoder returned an unexpected/overflowing stride");
        return false;
    }
    const std::size_t byte_count = stride * static_cast<std::size_t>(target_height);
    void* address = env->GetDirectBufferAddress(pixels);
    const jlong capacity = env->GetDirectBufferCapacity(pixels);
    if (address == nullptr || capacity < 0 ||
        static_cast<std::uint64_t>(capacity) < byte_count) {
        throw_fallback(env, "client AImageDecoder pixel buffer is truncated/non-direct");
        return false;
    }
    if (is_cancelled(env, cancellation)) {
        throw_cancelled(env);
        return false;
    }
    result = AImageDecoder_decodeImage(decoder, address, stride, byte_count);
    if (result != ANDROID_IMAGE_DECODER_SUCCESS) {
        throw_fallback(env, result_message("decodeImage", result));
        return false;
    }
    if (is_cancelled(env, cancellation)) {
        throw_cancelled(env);
        return false;
    }
    const int output_alpha = probe.alpha_flags == ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE
            ? ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE
            : (require_unpremultiplied == JNI_TRUE
                ? ANDROID_BITMAP_FLAGS_ALPHA_UNPREMUL
                : ANDROID_BITMAP_FLAGS_ALPHA_PREMUL);
    std::ostringstream out;
    out << kDecodeVersion << '\t' << target_width << '\t' << target_height
        << '\t' << bitmap_format << '\t' << output_alpha << '\t'
        << output_data_space << '\t' << stride << '\t' << byte_count;
    *response = out.str();
    return true;
}

jstring decode_fd_api30(JNIEnv* env, jint source_fd,
                        jstring declared_mime_value, jboolean allow_dng,
                        jstring expected_wire_value, jint target_width,
                        jint target_height, jint bitmap_format,
                        jint output_data_space, jboolean set_data_space,
                        jboolean require_unpremultiplied, jobject pixels,
                        jobject cancellation) SFA_REQUIRES_API(30) {
    std::string declared_mime;
    if (!get_declared_mime(env, declared_mime_value, &declared_mime)) {
        return nullptr;
    }
    std::string expected_wire;
    if (!get_nullable_utf8(env, expected_wire_value, &expected_wire) ||
        expected_wire.empty()) {
        if (!env->ExceptionCheck()) throw_fallback(env, "missing expected probe wire");
        return nullptr;
    }
    std::string error;
    OwnedFd fd = reopen_fd(source_fd, &error);
    if (!fd) {
        throw_fallback(env, error);
        return nullptr;
    }
    std::array<std::uint8_t, kSniffBytes> header{};
    std::uint64_t encoded_size = 0;
    std::size_t header_size = 0;
    if (!inspect_fd(&fd, &encoded_size, &header, &header_size, &error)) {
        throw_fallback(env, error);
        return nullptr;
    }
    const auto kind = sfaimage::sniff_input(header.data(), header_size, encoded_size);
    if (!kind) {
        throw_fallback(env, "encoded header changed after probe");
        return nullptr;
    }
    AImageDecoder* raw_decoder = nullptr;
    const int created = AImageDecoder_createFromFd(fd.get(), &raw_decoder);
    if (created != ANDROID_IMAGE_DECODER_SUCCESS || raw_decoder == nullptr) {
        throw_fallback(env, result_message("createFromFd", created));
        return nullptr;
    }
    DecoderOwner decoder(raw_decoder, &AImageDecoder_delete);
    Probe probe;
    if (!finish_probe(env, decoder.get(), *kind, encoded_size, declared_mime,
                      true, allow_dng == JNI_TRUE, &probe)) {
        return nullptr;
    }
    if (probe.wire() != expected_wire) {
        throw_fallback(env, "encoded source/header changed between probe and decode");
        return nullptr;
    }
    std::string response;
    if (!configure_and_decode(env, decoder.get(), probe, target_width,
                              target_height, bitmap_format, output_data_space,
                              set_data_space, require_unpremultiplied, pixels,
                              cancellation, &response)) {
        return nullptr;
    }
    return to_jstring(env, response);
}

jstring decode_buffer_api30(JNIEnv* env, jobject encoded,
                            jstring declared_mime_value, jboolean allow_dng,
                            jstring expected_wire_value, jint target_width,
                            jint target_height, jint bitmap_format,
                            jint output_data_space, jboolean set_data_space,
                            jboolean require_unpremultiplied, jobject pixels,
                            jobject cancellation) SFA_REQUIRES_API(30) {
    std::string declared_mime;
    if (!get_declared_mime(env, declared_mime_value, &declared_mime)) {
        return nullptr;
    }
    std::string expected_wire;
    if (!get_nullable_utf8(env, expected_wire_value, &expected_wire) ||
        expected_wire.empty()) {
        if (!env->ExceptionCheck()) throw_fallback(env, "missing expected probe wire");
        return nullptr;
    }
    DirectInput input;
    if (!inspect_direct_buffer(env, encoded, &input)) return nullptr;
    const auto kind = sfaimage::sniff_input(
            input.data, std::min(input.size, kSniffBytes), input.size);
    if (!kind) {
        throw_fallback(env, "encoded header changed after probe");
        return nullptr;
    }
    AImageDecoder* raw_decoder = nullptr;
    const int created = AImageDecoder_createFromBuffer(
            input.data, input.size, &raw_decoder);
    if (created != ANDROID_IMAGE_DECODER_SUCCESS || raw_decoder == nullptr) {
        throw_fallback(env, result_message("createFromBuffer", created));
        return nullptr;
    }
    DecoderOwner decoder(raw_decoder, &AImageDecoder_delete);
    Probe probe;
    if (!finish_probe(env, decoder.get(), *kind, input.size, declared_mime,
                      true, allow_dng == JNI_TRUE, &probe)) {
        return nullptr;
    }
    if (probe.wire() != expected_wire) {
        throw_fallback(env, "encoded source/header changed between probe and decode");
        return nullptr;
    }
    std::string response;
    if (!configure_and_decode(env, decoder.get(), probe, target_width,
                              target_height, bitmap_format, output_data_space,
                              set_data_space, require_unpremultiplied, pixels,
                              cancellation, &response)) {
        return nullptr;
    }
    return to_jstring(env, response);
}

jstring native_probe_fd(JNIEnv* env, jclass, jint fd, jstring declared_mime,
                        jboolean allow_dng) {
    return guard_jstring(env, [&]() -> jstring {
        if (__builtin_available(android 30, *)) {
            return probe_fd_api30(env, fd, declared_mime, allow_dng);
        }
        throw_fallback(env, "AImageDecoder requires Android API 30+");
        return nullptr;
    });
}

jstring native_probe_buffer(JNIEnv* env, jclass, jobject encoded,
                            jstring declared_mime, jboolean allow_dng) {
    return guard_jstring(env, [&]() -> jstring {
        if (__builtin_available(android 30, *)) {
            return probe_buffer_api30(env, encoded, declared_mime, allow_dng);
        }
        throw_fallback(env, "AImageDecoder requires Android API 30+");
        return nullptr;
    });
}

jstring native_decode_fd(JNIEnv* env, jclass, jint fd, jstring declared_mime,
                         jboolean allow_dng, jstring expected_wire, jint width,
                         jint height, jint format, jint data_space,
                         jboolean set_data_space, jboolean unpremultiplied,
                         jobject pixels, jobject cancellation) {
    return guard_jstring(env, [&]() -> jstring {
        if (__builtin_available(android 30, *)) {
            return decode_fd_api30(env, fd, declared_mime, allow_dng,
                                   expected_wire, width, height, format,
                                   data_space, set_data_space, unpremultiplied,
                                   pixels, cancellation);
        }
        throw_fallback(env, "AImageDecoder requires Android API 30+");
        return nullptr;
    });
}

jstring native_decode_buffer(JNIEnv* env, jclass, jobject encoded,
                             jstring declared_mime, jboolean allow_dng,
                             jstring expected_wire, jint width, jint height,
                             jint format, jint data_space,
                             jboolean set_data_space, jboolean unpremultiplied,
                             jobject pixels, jobject cancellation) {
    return guard_jstring(env, [&]() -> jstring {
        if (__builtin_available(android 30, *)) {
            return decode_buffer_api30(env, encoded, declared_mime, allow_dng,
                                       expected_wire, width, height, format,
                                       data_space, set_data_space,
                                       unpremultiplied, pixels, cancellation);
        }
        throw_fallback(env, "AImageDecoder requires Android API 30+");
        return nullptr;
    });
}

float srgb_to_linear(float encoded) {
    if (encoded <= 0.04045F) return encoded / 12.92F;
    const double base = (static_cast<double>(encoded) + 0.055) / 1.055;
    return static_cast<float>(std::pow(base, 2.4));
}

void native_convert_srgb(JNIEnv* env, jclass, jobject rgba, jint rgba_stride,
                         jobject rgb_float, jint width, jint height,
                         jobject cancellation) {
    guard_void(env, [&]() {
        if (width <= 0 || height <= 0 || rgba_stride <= 0) {
            throw_fallback(env, "invalid sRGB conversion geometry");
            return;
        }
        const std::uint64_t row_bytes = static_cast<std::uint64_t>(width) * 4U;
        if (row_bytes > static_cast<std::uint64_t>(rgba_stride)) {
            throw_fallback(env, "sRGB conversion stride is truncated");
            return;
        }
        const std::uint64_t rgba_bytes =
                static_cast<std::uint64_t>(rgba_stride) * (height - 1ULL) +
                row_bytes;
        const std::uint64_t pixel_count =
                static_cast<std::uint64_t>(width) *
                static_cast<std::uint64_t>(height);
        constexpr std::uint64_t kRgbFloatBytes = 3ULL * sizeof(float);
        if (pixel_count >
            std::numeric_limits<std::uint64_t>::max() / kRgbFloatBytes) {
            throw_fallback(env, "sRGB conversion output size overflow");
            return;
        }
        const std::uint64_t rgb_bytes = pixel_count * kRgbFloatBytes;
        if (rgba_bytes > std::numeric_limits<std::size_t>::max() ||
            rgb_bytes > std::numeric_limits<std::size_t>::max()) {
            throw_fallback(env, "sRGB conversion exceeds the native address space");
            return;
        }
        const jlong rgba_capacity = env->GetDirectBufferCapacity(rgba);
        const jlong rgb_capacity = env->GetDirectBufferCapacity(rgb_float);
        auto* source =
                static_cast<const std::uint8_t*>(env->GetDirectBufferAddress(rgba));
        auto* target = static_cast<float*>(env->GetDirectBufferAddress(rgb_float));
        if (source == nullptr || target == nullptr || rgba_capacity < 0 ||
            rgb_capacity < 0 ||
            static_cast<std::uint64_t>(rgba_capacity) < rgba_bytes ||
            static_cast<std::uint64_t>(rgb_capacity) < rgb_bytes) {
            throw_fallback(env, "sRGB conversion requires complete direct buffers");
            return;
        }
        if (is_cancelled(env, cancellation)) {
            throw_cancelled(env);
            return;
        }
        constexpr float m[9] = {
            0.5290825F, 0.3303437F, 0.1405738F,
            0.0982640F, 0.8734031F, 0.0283329F,
            0.0167029F, 0.1176946F, 0.8656026F,
        };
        for (int y = 0; y < height; ++y) {
            if ((y & 15) == 0 && is_cancelled(env, cancellation)) {
                throw_cancelled(env);
                return;
            }
            const auto* row = source + static_cast<std::size_t>(y) * rgba_stride;
            std::size_t output = static_cast<std::size_t>(y) * width * 3U;
            for (int x = 0; x < width; ++x) {
                const float r = srgb_to_linear(row[x * 4U] / 255.0F);
                const float g = srgb_to_linear(row[x * 4U + 1U] / 255.0F);
                const float b = srgb_to_linear(row[x * 4U + 2U] / 255.0F);
                target[output] = m[0] * r + m[1] * g + m[2] * b;
                target[output + 1U] = m[3] * r + m[4] * g + m[5] * b;
                target[output + 2U] = m[6] * r + m[7] * g + m[8] * b;
                output += 3U;
            }
        }
        if (is_cancelled(env, cancellation)) throw_cancelled(env);
    });
}

jlongArray native_debug_outstanding(JNIEnv* env, jclass) {
    jlongArray result = env->NewLongArray(2);
    if (result == nullptr) return nullptr;
    const jlong values[2] = {
        g_live_decoders.load(std::memory_order_relaxed),
        g_live_fds.load(std::memory_order_relaxed),
    };
    env->SetLongArrayRegion(result, 0, 2, values);
    return result;
}

const JNINativeMethod kMethods[] = {
    {const_cast<char*>("probeFd"),
     const_cast<char*>("(ILjava/lang/String;Z)Ljava/lang/String;"),
     reinterpret_cast<void*>(native_probe_fd)},
    {const_cast<char*>("probeBuffer"),
     const_cast<char*>("(Ljava/nio/ByteBuffer;Ljava/lang/String;Z)Ljava/lang/String;"),
     reinterpret_cast<void*>(native_probe_buffer)},
    {const_cast<char*>("decodeFd"),
     const_cast<char*>("(ILjava/lang/String;ZLjava/lang/String;IIIIZZLjava/nio/ByteBuffer;Lcom/spectrafilm/aimage/AImageDecodeCancellation;)Ljava/lang/String;"),
     reinterpret_cast<void*>(native_decode_fd)},
    {const_cast<char*>("decodeBuffer"),
     const_cast<char*>("(Ljava/nio/ByteBuffer;Ljava/lang/String;ZLjava/lang/String;IIIIZZLjava/nio/ByteBuffer;Lcom/spectrafilm/aimage/AImageDecodeCancellation;)Ljava/lang/String;"),
     reinterpret_cast<void*>(native_decode_buffer)},
    {const_cast<char*>("convertSrgb8888ToLinearProPhoto"),
     const_cast<char*>("(Ljava/nio/ByteBuffer;ILjava/nio/ByteBuffer;IILcom/spectrafilm/aimage/AImageDecodeCancellation;)V"),
     reinterpret_cast<void*>(native_convert_srgb)},
    {const_cast<char*>("debugOutstandingResources"),
     const_cast<char*>("()[J"),
     reinterpret_cast<void*>(native_debug_outstanding)},
};

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK ||
        env == nullptr) {
        return JNI_ERR;
    }
    jclass bridge = env->FindClass("com/spectrafilm/aimage/AImageDecoderNative");
    if (bridge == nullptr) return JNI_ERR;
    jmethodID fallback_class_method = env->GetStaticMethodID(
            bridge, "fallbackExceptionClass", "()Ljava/lang/Class;");
    if (fallback_class_method == nullptr) {
        env->DeleteLocalRef(bridge);
        return JNI_ERR;
    }
    auto fallback_local = static_cast<jclass>(
            env->CallStaticObjectMethod(bridge, fallback_class_method));
    if (fallback_local == nullptr || env->ExceptionCheck()) {
        env->DeleteLocalRef(bridge);
        return JNI_ERR;
    }
    g_fallback_class = static_cast<jclass>(env->NewGlobalRef(fallback_local));
    env->DeleteLocalRef(fallback_local);
    if (g_fallback_class == nullptr) {
        env->DeleteLocalRef(bridge);
        return JNI_ERR;
    }
    const jint registered = env->RegisterNatives(
            bridge, kMethods, static_cast<jint>(std::size(kMethods)));
    env->DeleteLocalRef(bridge);
    if (registered != JNI_OK) {
        env->DeleteGlobalRef(g_fallback_class);
        g_fallback_class = nullptr;
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}

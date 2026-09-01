/* Spektrafilm Android — dependency-free encoded-image gate. GPL-3.0-only. */
#include "image_input_gate.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

namespace sfaimage {
namespace {

bool starts_with(const std::uint8_t* data, std::size_t size,
                 std::initializer_list<std::uint8_t> bytes) noexcept {
    if (size < bytes.size()) return false;
    return std::equal(bytes.begin(), bytes.end(), data);
}

bool equals_ascii(const std::uint8_t* data, const char (&literal)[5]) noexcept {
    return data[0] == static_cast<std::uint8_t>(literal[0]) &&
           data[1] == static_cast<std::uint8_t>(literal[1]) &&
           data[2] == static_cast<std::uint8_t>(literal[2]) &&
           data[3] == static_cast<std::uint8_t>(literal[3]);
}

bool is_heif_brand(const std::uint8_t* brand) noexcept {
    return equals_ascii(brand, "heic") || equals_ascii(brand, "heix") ||
           equals_ascii(brand, "hevc") || equals_ascii(brand, "hevx") ||
           equals_ascii(brand, "heim") || equals_ascii(brand, "heis") ||
           equals_ascii(brand, "mif1") || equals_ascii(brand, "msf1");
}

std::optional<std::uint32_t> read_wbmp_multibyte(const std::uint8_t* data,
                                                 std::size_t size,
                                                 std::size_t* cursor) noexcept {
    std::uint32_t value = 0;
    for (int count = 0; count < 5; ++count) {
        if (*cursor >= size) return std::nullopt;
        const std::uint8_t octet = data[(*cursor)++];
        if (value > (std::numeric_limits<std::uint32_t>::max() >> 7U)) {
            return std::nullopt;
        }
        value = (value << 7U) | (octet & 0x7FU);
        if ((octet & 0x80U) == 0U) return value;
    }
    return std::nullopt;
}

bool looks_like_wbmp(const std::uint8_t* data, std::size_t size,
                     std::uint64_t encoded_size) noexcept {
    if (size < 4 || data[0] != 0 || data[1] != 0) return false;
    std::size_t cursor = 2;
    const auto width = read_wbmp_multibyte(data, size, &cursor);
    const auto height = read_wbmp_multibyte(data, size, &cursor);
    if (!width || !height || *width == 0 || *height == 0) return false;
    const std::uint64_t row_bytes = (static_cast<std::uint64_t>(*width) + 7U) / 8U;
    if (*height > std::numeric_limits<std::uint64_t>::max() / row_bytes) {
        return false;
    }
    const std::uint64_t payload = row_bytes * *height;
    return cursor <= encoded_size && payload <= encoded_size - cursor;
}

}  // namespace

std::optional<InputKind> sniff_input(const std::uint8_t* header,
                                     std::size_t header_size,
                                     std::uint64_t encoded_size) noexcept {
    if (header == nullptr || encoded_size == 0 ||
        encoded_size > kMaxEncodedBytes || header_size == 0) {
        return std::nullopt;
    }
    if (starts_with(header, header_size, {0xFF, 0xD8, 0xFF})) {
        return InputKind::Jpeg;
    }
    if (starts_with(header, header_size,
                    {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A})) {
        return InputKind::Png;
    }
    if (header_size >= 6 &&
        (std::equal(header, header + 6,
                    reinterpret_cast<const std::uint8_t*>("GIF87a")) ||
         std::equal(header, header + 6,
                    reinterpret_cast<const std::uint8_t*>("GIF89a")))) {
        return InputKind::Gif;
    }
    if (header_size >= 12 &&
        std::equal(header, header + 4,
                   reinterpret_cast<const std::uint8_t*>("RIFF")) &&
        std::equal(header + 8, header + 12,
                   reinterpret_cast<const std::uint8_t*>("WEBP"))) {
        return InputKind::Webp;
    }
    if (starts_with(header, header_size, {0x42, 0x4D})) {
        return InputKind::Bmp;
    }
    if (starts_with(header, header_size, {0x00, 0x00, 0x01, 0x00})) {
        return InputKind::Ico;
    }
    if (header_size >= 12 &&
        std::equal(header + 4, header + 8,
                   reinterpret_cast<const std::uint8_t*>("ftyp"))) {
        if (is_heif_brand(header + 8)) return InputKind::Heif;
        // major_brand, minor_version, then compatible brands.
        for (std::size_t offset = 16; offset + 4 <= header_size; offset += 4) {
            if (is_heif_brand(header + offset)) return InputKind::Heif;
        }
    }
    if (starts_with(header, header_size, {0x49, 0x49, 0x2A, 0x00}) ||
        starts_with(header, header_size, {0x4D, 0x4D, 0x00, 0x2A})) {
        // The platform decoder and explicit MIME/fallback flag must still prove DNG.
        return InputKind::Dng;
    }
    if (looks_like_wbmp(header, header_size, encoded_size)) {
        return InputKind::Wbmp;
    }
    return std::nullopt;
}

const char* kind_name(InputKind kind) noexcept {
    switch (kind) {
        case InputKind::Jpeg: return "JPEG";
        case InputKind::Png: return "PNG";
        case InputKind::Gif: return "GIF";
        case InputKind::Webp: return "WEBP";
        case InputKind::Bmp: return "BMP";
        case InputKind::Ico: return "ICO";
        case InputKind::Wbmp: return "WBMP";
        case InputKind::Heif: return "HEIF";
        case InputKind::Dng: return "DNG";
    }
    return "UNKNOWN";
}

bool mime_matches(InputKind kind, std::string_view mime) noexcept {
    switch (kind) {
        case InputKind::Jpeg: return mime == "image/jpeg";
        case InputKind::Png: return mime == "image/png";
        case InputKind::Gif: return mime == "image/gif";
        case InputKind::Webp: return mime == "image/webp";
        case InputKind::Bmp:
            return mime == "image/bmp" || mime == "image/x-ms-bmp";
        case InputKind::Ico:
            return mime == "image/x-ico" || mime == "image/x-icon" ||
                   mime == "image/vnd.microsoft.icon" || mime == "image/ico";
        case InputKind::Wbmp: return mime == "image/vnd.wap.wbmp";
        case InputKind::Heif:
            return mime == "image/heif" || mime == "image/heic";
        case InputKind::Dng: return mime == "image/x-adobe-dng";
    }
    return false;
}

bool declared_mime_allows(InputKind kind, std::string_view declared_mime,
                          bool allow_dng_fallback) noexcept {
    if (kind == InputKind::Dng) {
        return allow_dng_fallback && declared_mime == "image/x-adobe-dng";
    }
    return declared_mime.empty() || mime_matches(kind, declared_mime);
}

}  // namespace sfaimage

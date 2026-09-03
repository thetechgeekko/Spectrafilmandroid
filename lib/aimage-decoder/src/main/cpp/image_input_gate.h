/* Spektrafilm Android — dependency-free encoded-image gate. GPL-3.0-only. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace sfaimage {

enum class InputKind {
    Jpeg,
    Png,
    Gif,
    Webp,
    Bmp,
    Ico,
    Wbmp,
    Heif,
    Dng,
};

constexpr std::uint64_t kMaxEncodedBytes = 128ULL * 1024ULL * 1024ULL;

std::optional<InputKind> sniff_input(const std::uint8_t* header,
                                     std::size_t header_size,
                                     std::uint64_t encoded_size) noexcept;

const char* kind_name(InputKind kind) noexcept;

bool mime_matches(InputKind kind, std::string_view mime) noexcept;

/** Empty declared MIME is allowed except for TIFF-signature DNG fallback. */
bool declared_mime_allows(InputKind kind, std::string_view declared_mime,
                          bool allow_dng_fallback) noexcept;

}  // namespace sfaimage

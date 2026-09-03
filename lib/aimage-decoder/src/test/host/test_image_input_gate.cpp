/* Spektrafilm Android — AImageDecoder header/MIME gate tests. GPL-3.0-only. */
#include "image_input_gate.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        ++failures;
    }
}

template <std::size_t N>
void expect_kind(const std::array<std::uint8_t, N>& bytes,
                 std::uint64_t encoded_size, sfaimage::InputKind expected,
                 std::string_view label) {
    const auto actual = sfaimage::sniff_input(bytes.data(), bytes.size(), encoded_size);
    check(actual.has_value() && *actual == expected, label);
}

}  // namespace

int main() {
    expect_kind(std::array<std::uint8_t, 4>{0xFF, 0xD8, 0xFF, 0xE0}, 4,
                sfaimage::InputKind::Jpeg, "JPEG signature");
    expect_kind(std::array<std::uint8_t, 8>{
                    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}, 8,
                sfaimage::InputKind::Png, "PNG signature");
    expect_kind(std::array<std::uint8_t, 6>{'G', 'I', 'F', '8', '9', 'a'}, 6,
                sfaimage::InputKind::Gif, "GIF89a signature");
    expect_kind(std::array<std::uint8_t, 12>{
                    'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'E', 'B', 'P'}, 12,
                sfaimage::InputKind::Webp, "WebP RIFF signature");
    expect_kind(std::array<std::uint8_t, 4>{'B', 'M', 0, 0}, 4,
                sfaimage::InputKind::Bmp, "BMP signature");
    expect_kind(std::array<std::uint8_t, 6>{0, 0, 1, 0, 1, 0}, 6,
                sfaimage::InputKind::Ico, "ICO signature");
    expect_kind(std::array<std::uint8_t, 8>{0, 0, 8, 1, 0xAA, 0, 0, 0}, 8,
                sfaimage::InputKind::Wbmp, "bounded WBMP header/payload");
    expect_kind(std::array<std::uint8_t, 20>{
                    0, 0, 0, 20, 'f', 't', 'y', 'p', 'm', 'i', 'f', '1',
                    0, 0, 0, 0, 'h', 'e', 'i', 'c'}, 20,
                sfaimage::InputKind::Heif, "HEIF compatible brand");
    expect_kind(std::array<std::uint8_t, 8>{'I', 'I', 0x2A, 0, 8, 0, 0, 0}, 8,
                sfaimage::InputKind::Dng, "little-endian TIFF/DNG gate");

    const std::array<std::uint8_t, 4> unknown{1, 2, 3, 4};
    check(!sfaimage::sniff_input(unknown.data(), unknown.size(), unknown.size()),
          "unknown header rejected");
    check(!sfaimage::sniff_input(unknown.data(), unknown.size(),
                                 sfaimage::kMaxEncodedBytes + 1),
          "oversized encoded input rejected before decoder");
    const std::array<std::uint8_t, 4> truncated_wbmp{0, 0, 8, 8};
    check(!sfaimage::sniff_input(truncated_wbmp.data(), truncated_wbmp.size(),
                                 truncated_wbmp.size()),
          "WBMP with missing payload rejected");

    check(sfaimage::mime_matches(sfaimage::InputKind::Ico,
                                 "image/vnd.microsoft.icon"),
          "ICO platform MIME alias accepted");
    check(sfaimage::mime_matches(sfaimage::InputKind::Ico, "image/x-ico"),
          "Android ImageDecoder ICO MIME accepted");
    check(!sfaimage::mime_matches(sfaimage::InputKind::Png, "image/jpeg"),
          "platform MIME/header mismatch rejected");
    check(sfaimage::declared_mime_allows(sfaimage::InputKind::Png, {}, false),
          "empty non-DNG declared MIME allowed with header/platform agreement");
    check(!sfaimage::declared_mime_allows(sfaimage::InputKind::Dng,
                                          "image/x-adobe-dng", false),
          "DNG needs explicit display-fallback flag");
    check(!sfaimage::declared_mime_allows(sfaimage::InputKind::Dng, {}, true),
          "DNG needs explicit declared MIME");
    check(sfaimage::declared_mime_allows(sfaimage::InputKind::Dng,
                                         "image/x-adobe-dng", true),
          "explicit DNG display-fallback contract accepted");

    if (failures == 0) {
        std::cout << "AIMAGE_INPUT_GATE: PASS\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}

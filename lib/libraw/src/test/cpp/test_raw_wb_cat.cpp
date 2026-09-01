/*
 * Exact host/device golden for the RAW WB production math in raw_decoder.cpp.
 *
 * raw_wb_cat_fixture.h is generated at build time from the immutable research
 * JSON after its canonical SHA-256 and captured colour-reference bits pass.
 * This test therefore consumes the same fixture without adding a JSON parser to
 * the shipping native library or duplicating hand-maintained expected values.
 */
#include "raw_decoder.h"
#include "raw_wb_cat_fixture.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace {

using sfraw::raw_wb_fixture::Vector;

std::uint32_t bitsOf(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float floatOf(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void writeU32(std::ofstream& output, std::uint32_t value) {
    const unsigned char bytes[4] = {
        static_cast<unsigned char>(value),
        static_cast<unsigned char>(value >> 8),
        static_cast<unsigned char>(value >> 16),
        static_cast<unsigned char>(value >> 24),
    };
    output.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

bool sameBits(const float values[3], const std::uint32_t expected[3],
              const Vector& fixture, const char* boundary) {
    bool ok = true;
    for (int channel = 0; channel < 3; ++channel) {
        const std::uint32_t got = bitsOf(values[channel]);
        if (got != expected[channel]) {
            std::cerr << "FAIL " << fixture.scenario << '/' << fixture.patch
                      << ' ' << boundary << " channel=" << channel
                      << " got=0x" << std::hex << got
                      << " expected=0x" << expected[channel] << std::dec
                      << '\n';
            ok = false;
        }
    }
    return ok;
}

spectrafilm::DecodeOptions optionsFor(const Vector& fixture) {
    spectrafilm::DecodeOptions options;
    options.whiteBalance =
        static_cast<spectrafilm::WhiteBalanceMode>(fixture.mode);
    options.temperatureK = fixture.temperature_k;
    options.tint = fixture.tint;
    return options;
}

bool rejectsWithoutMutation(spectrafilm::DecodeOptions options) {
    float values[3] = {0.25f, -0.5f, 2.0f};
    const std::uint32_t before[3] = {
        bitsOf(values[0]), bitsOf(values[1]), bitsOf(values[2]),
    };
    if (spectrafilm::rawWhiteBalanceOptionsValid(options)) return false;
    if (spectrafilm::applyAcesWhiteBalance(values, 1, options)) return false;
    return bitsOf(values[0]) == before[0] &&
           bitsOf(values[1]) == before[1] &&
           bitsOf(values[2]) == before[2];
}

bool writeDeviceEvidence(const std::string& path,
                         const std::vector<std::uint32_t>& outputBits) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write("SFRWBCAT", 8);
    output.write(sfraw::raw_wb_fixture::kReportSha256, 64);
    writeU32(output, static_cast<std::uint32_t>(
                         sfraw::raw_wb_fixture::kVectorCount));
    writeU32(output, static_cast<std::uint32_t>(outputBits.size()));
    for (const std::uint32_t bits : outputBits) writeU32(output, bits);
    return static_cast<bool>(output);
}

}  // namespace

int main(int argc, char** argv) {
    std::string dumpPath;
    if (argc == 3 && std::string(argv[1]) == "--dump") {
        dumpPath = argv[2];
    } else if (argc != 1) {
        std::cerr << "usage: sfraw_raw_wb_cat_test [--dump OUTPUT]\n";
        return 2;
    }

    constexpr char kPinnedReportSha256[] =
        "fff1b77b0dfd776fcf6eefd24451166affb2f465695c5b89d0edc13453dc3d09";
    if (std::strcmp(sfraw::raw_wb_fixture::kReportSha256,
                    kPinnedReportSha256) != 0) {
        std::cerr << "FAIL generated fixture digest is not the reviewed baseline\n";
        return 1;
    }
    if (sfraw::raw_wb_fixture::kVectorCount != 56U) {
        std::cerr << "FAIL expected all 8 scenarios x 7 patches, got "
                  << sfraw::raw_wb_fixture::kVectorCount << '\n';
        return 1;
    }

    int failures = 0;
    bool sawSingleRoundTintDivergence = false;
    bool sawReferenceSkipDivergence = false;
    bool sawTintSkipDivergence = false;
    bool sawNeutral = false;
    bool sawHdr = false;
    bool sawWideGamut = false;
    std::set<std::string> scenarios;
    std::set<std::string> patches;
    std::set<std::string> scenarioPatchPairs;
    std::vector<std::uint32_t> evidenceBits;
    evidenceBits.reserve(sfraw::raw_wb_fixture::kVectorCount * 6U);

    for (const Vector& fixture : sfraw::raw_wb_fixture::kVectors) {
        scenarios.insert(fixture.scenario);
        patches.insert(fixture.patch);
        scenarioPatchPairs.insert(std::string(fixture.scenario) + "/" +
                                  fixture.patch);
        const std::string patch = fixture.patch;
        sawNeutral = sawNeutral || patch.rfind("neutral_", 0) == 0;
        sawHdr = sawHdr || patch == "neutral_hdr" || patch == "wide_gamut_hdr";
        sawWideGamut = sawWideGamut || patch == "wide_gamut_hdr";

        float values[3] = {
            floatOf(fixture.input_aces_bits[0]),
            floatOf(fixture.input_aces_bits[1]),
            floatOf(fixture.input_aces_bits[2]),
        };
        const spectrafilm::DecodeOptions options = optionsFor(fixture);
        if (!spectrafilm::rawWhiteBalanceOptionsValid(options) ||
            !spectrafilm::applyAcesWhiteBalance(values, 1U, options)) {
            std::cerr << "FAIL valid vector rejected " << fixture.scenario
                      << '/' << fixture.patch << '\n';
            ++failures;
            continue;
        }
        if (!sameBits(values, fixture.expected_aces_bits, fixture,
                      "ACES-after-WB")) {
            ++failures;
        }
        if (fixture.mode == 0 || fixture.mode == 1) {
            for (int channel = 0; channel < 3; ++channel) {
                if (bitsOf(values[channel]) != fixture.input_aces_bits[channel]) {
                    std::cerr << "FAIL as-shot/daylight changed bytes "
                              << fixture.scenario << '/' << fixture.patch << '\n';
                    ++failures;
                    break;
                }
            }
        }
        for (int channel = 0; channel < 3; ++channel) {
            sawSingleRoundTintDivergence = sawSingleRoundTintDivergence ||
                fixture.expected_aces_bits[channel] !=
                    fixture.wrong_single_round_tint_aces_bits[channel];
            sawReferenceSkipDivergence = sawReferenceSkipDivergence ||
                fixture.expected_aces_bits[channel] !=
                    fixture.wrong_no_reference_skip_aces_bits[channel];
            sawTintSkipDivergence = sawTintSkipDivergence ||
                fixture.expected_aces_bits[channel] !=
                    fixture.wrong_no_tint_skip_aces_bits[channel];
            evidenceBits.push_back(bitsOf(values[channel]));
        }

        spectrafilm::aces2065ToProPhotoRGB(values, 1U);
        if (!sameBits(values, fixture.expected_prophoto_bits, fixture,
                      "ProPhoto-f32")) {
            ++failures;
        }
        for (float value : values) evidenceBits.push_back(bitsOf(value));
    }

    if (scenarios.size() != 8U || patches.size() != 7U ||
        scenarioPatchPairs.size() != 56U || !sawNeutral || !sawHdr ||
        !sawWideGamut || !sawSingleRoundTintDivergence ||
        !sawReferenceSkipDivergence || !sawTintSkipDivergence) {
        std::cerr << "FAIL fixture coverage/diagnostic candidates are incomplete\n";
        ++failures;
    }

    spectrafilm::DecodeOptions boundary;
    boundary.whiteBalance = spectrafilm::WhiteBalanceMode::Custom;
    for (const double temperature : {1000.0, 12000.0}) {
        for (const double tint : {0.2, 1.8}) {
            boundary.temperatureK = temperature;
            boundary.tint = tint;
            if (!spectrafilm::rawWhiteBalanceOptionsValid(boundary)) {
                std::cerr << "FAIL documented product boundary rejected\n";
                ++failures;
            }
        }
    }

    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (const double invalidTemperature :
         {nan, infinity, -infinity, 0.0, -1.0, 999.999, 12000.001}) {
        spectrafilm::DecodeOptions invalid;
        invalid.whiteBalance = spectrafilm::WhiteBalanceMode::Custom;
        invalid.temperatureK = invalidTemperature;
        if (!rejectsWithoutMutation(invalid)) {
            std::cerr << "FAIL invalid temperature accepted\n";
            ++failures;
        }
    }
    for (const double invalidTint :
         {nan, infinity, -infinity, -1.0, 0.0, 0.199999, 1.800001}) {
        spectrafilm::DecodeOptions invalid;
        invalid.whiteBalance = spectrafilm::WhiteBalanceMode::Custom;
        invalid.tint = invalidTint;
        if (!rejectsWithoutMutation(invalid)) {
            std::cerr << "FAIL invalid tint accepted\n";
            ++failures;
        }
    }
    for (int mode = 0; mode < 4; ++mode) {
        spectrafilm::DecodeOptions invalid;
        invalid.whiteBalance = static_cast<spectrafilm::WhiteBalanceMode>(mode);
        invalid.temperatureK = nan;
        if (!rejectsWithoutMutation(invalid)) {
            std::cerr << "FAIL non-finite unused setting accepted for mode="
                      << mode << '\n';
            ++failures;
        }
    }
    {
        spectrafilm::DecodeOptions invalid;
        invalid.whiteBalance = static_cast<spectrafilm::WhiteBalanceMode>(99);
        if (!rejectsWithoutMutation(invalid)) {
            std::cerr << "FAIL unknown native WB mode accepted\n";
            ++failures;
        }
    }
    {
        spectrafilm::DecodeOptions invalid;
        invalid.whiteBalance = spectrafilm::WhiteBalanceMode::Custom;
        invalid.temperatureK = nan;
        const std::uint8_t input = 0;
        const spectrafilm::DecodeResult result =
            spectrafilm::decodeFromBuffer(&input, 1U, invalid);
        if (result.status != spectrafilm::SFRAW_ERR_INPUT) {
            std::cerr << "FAIL buffer decode seam did not return typed INPUT\n";
            ++failures;
        }
        const spectrafilm::DecodeResult fdResult =
            spectrafilm::decodeFromFd(-1, invalid);
        if (fdResult.status != spectrafilm::SFRAW_ERR_INPUT) {
            std::cerr << "FAIL fd decode seam did not return typed INPUT\n";
            ++failures;
        }
    }

    if (!dumpPath.empty() && !writeDeviceEvidence(dumpPath, evidenceBits)) {
        std::cerr << "FAIL could not write device evidence " << dumpPath << '\n';
        ++failures;
    }

    if (failures != 0) {
        std::cerr << "RAW-WB CAT02 FAIL failures=" << failures << '\n';
        return 1;
    }
    std::cout << "RAW-WB CAT02 PASS vectors="
              << sfraw::raw_wb_fixture::kVectorCount
              << " scenarios=" << scenarios.size()
              << " patches=" << patches.size()
              << " report_sha256=" << sfraw::raw_wb_fixture::kReportSha256
              << (dumpPath.empty() ? "" : " evidence=" + dumpPath)
              << '\n';
    return 0;
}

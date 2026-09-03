/* Spektrafilm for Android -- hostile-input regression tests for the NPY LUT loader. */
#include "io/npy_lut.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr size_t kFileLimit = 32u * 1024u * 1024u;
constexpr size_t kHeaderLimit = 10'000u;

std::vector<char> npy(uint8_t major, uint8_t minor, const std::string& header,
                      const std::vector<char>& payload = {}) {
    if (major == 1 && header.size() > UINT16_MAX)
        throw std::runtime_error("test v1 header too large");
    if (header.size() > UINT32_MAX) throw std::runtime_error("test header too large");
    std::vector<char> bytes = {
        '\x93', 'N', 'U', 'M', 'P', 'Y', static_cast<char>(major),
        static_cast<char>(minor)};
    const uint32_t header_size = static_cast<uint32_t>(header.size());
    bytes.push_back(static_cast<char>(header_size & 0xffu));
    bytes.push_back(static_cast<char>((header_size >> 8) & 0xffu));
    if (major == 2) {
        bytes.push_back(static_cast<char>((header_size >> 16) & 0xffu));
        bytes.push_back(static_cast<char>((header_size >> 24) & 0xffu));
    }
    bytes.insert(bytes.end(), header.begin(), header.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::vector<char> npy_v1(const std::string& header,
                         const std::vector<char>& payload = {}) {
    return npy(1, 0, header, payload);
}

std::string header(const std::string& descr, const std::string& order,
                   const std::string& shape) {
    return "{'descr': '" + descr + "', 'fortran_order': " + order +
           ", 'shape': (" + shape + "), }\n";
}

std::string aligned_header(uint8_t major, const std::string& descr,
                           const std::string& order, const std::string& shape) {
    std::string result = "{'descr': '" + descr + "', 'fortran_order': " + order +
                         ", 'shape': (" + shape + "), }";
    const size_t prefix_size = major == 1 ? 10u : 12u;
    const size_t padding = (64u - ((prefix_size + result.size() + 1u) % 64u)) % 64u;
    result.append(padding, ' ');
    result.push_back('\n');
    return result;
}

std::vector<char> little_endian(uint64_t bits, size_t width) {
    std::vector<char> result(width);
    for (size_t i = 0; i < width; ++i)
        result[i] = static_cast<char>((bits >> (i * 8u)) & 0xffu);
    return result;
}

bool expect_rejected(const char* label, const std::vector<char>& bytes,
                     const char* expected_message) {
    try {
        (void)spk::parse_npy(bytes.data(), bytes.size(), label);
    } catch (const std::runtime_error& error) {
        if (std::strstr(error.what(), expected_message) != nullptr) return true;
        std::fprintf(stderr, "%s: wrong error: %s\n", label, error.what());
        return false;
    }
    std::fprintf(stderr, "%s: hostile input was accepted\n", label);
    return false;
}

bool expect_value(const char* label, const std::vector<char>& bytes,
                  double expected) {
    try {
        const spk::NdArray parsed = spk::parse_npy(bytes.data(), bytes.size(), label);
        if (parsed.shape == std::vector<int>({1}) && parsed.data.size() == 1u &&
            parsed.data[0] == expected) {
            return true;
        }
        std::fprintf(stderr, "%s: decoded value/shape mismatch\n", label);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: valid input rejected: %s\n", label, error.what());
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    bool ok = true;
    ok &= expect_value("valid-v1-f16.npy",
                       npy(1, 0, aligned_header(1, "<f2", "False", "1,"),
                           little_endian(0x3c00u, 2)),
                       1.0);
    ok &= expect_value("valid-v2-f32.npy",
                       npy(2, 0, aligned_header(2, "<f4", "False", "1,"),
                           little_endian(0xc0200000u, 4)),
                       -2.5);
    ok &= expect_value("valid-v1-f64.npy",
                       npy(1, 0, aligned_header(1, "<f8", "False", "1,"),
                           little_endian(0x400a000000000000ull, 8)),
                       3.25);

    ok &= expect_rejected(
        "malformed-shape.npy",
        npy_v1(header("<f4", "False", "2, nope,")), "malformed shape");
    ok &= expect_rejected("negative-shape.npy",
                          npy_v1(header("<f4", "False", "-1,")),
                          "malformed shape");
    ok &= expect_rejected("zero-shape.npy", npy_v1(header("<f4", "False", "0,")),
                          "zero dimension");
    ok &= expect_rejected(
        "missing-shape-close.npy",
        npy_v1("{'descr': '<f4', 'fortran_order': False, 'shape': (2, 2, }\n"),
        "malformed header");
    ok &= expect_rejected(
        "missing-descr-quote.npy",
        npy_v1("{'descr': '<f4, 'fortran_order': False, 'shape': (1,), }\n"),
        "malformed header");
    ok &= expect_rejected(
        "delayed-colon.npy",
        npy_v1("{'descr' nonsense: '<f4', 'fortran_order': False, "
               "'shape': (1,), }\n", std::vector<char>(4)),
        "malformed header");
    ok &= expect_rejected(
        "fake-key.npy",
        npy_v1("{'fake': \"'descr': '<f4'\", 'fortran_order': False, "
               "'shape': (1,), }\n", std::vector<char>(4)),
        "malformed header");
    ok &= expect_rejected(
        "duplicate-key.npy",
        npy_v1("{'descr': '<f4', 'descr': '>f4', 'fortran_order': False, "
               "'shape': (1,), }\n", std::vector<char>(4)),
        "malformed header");
    ok &= expect_rejected(
        "unknown-key.npy",
        npy_v1("{'descr': '<f4', 'fortran_order': False, 'shape': (1,), "
               "'junk': 1, }\n", std::vector<char>(4)),
        "malformed header");
    ok &= expect_rejected(
        "bare-descr.npy",
        npy_v1("{'descr': <f4, 'fortran_order': False, 'shape': (1,), }\n",
               std::vector<char>(4)),
        "malformed header");
    ok &= expect_rejected(
        "quoted-order.npy",
        npy_v1("{'descr': '<f4', 'fortran_order': 'False', "
               "'shape': (1,), }\n", std::vector<char>(4)),
        "malformed header");
    ok &= expect_rejected(
        "quoted-shape.npy",
        npy_v1("{'descr': '<f4', 'fortran_order': False, "
               "'shape': '1,', }\n", std::vector<char>(4)),
        "malformed header");
    ok &= expect_rejected("rank-one-not-tuple.npy",
                          npy_v1(header("<f4", "False", "1"),
                                 std::vector<char>(4)),
                          "malformed shape");
    ok &= expect_rejected("version-1.1.npy",
                          npy(1, 1, header("<f4", "False", "1,"),
                              std::vector<char>(4)),
                          "unsupported .npy version");
    ok &= expect_rejected("invalid-order.npy",
                          npy_v1(header("<f4", "Falsehood", "1,"),
                                 std::vector<char>(4)),
                          "invalid fortran_order");
    ok &= expect_rejected("fortran.npy", npy_v1(header("<f4", "True", "1,")),
                          "fortran_order not supported");
    ok &= expect_rejected("big-endian.npy",
                          npy_v1(header(">f4", "False", "1,")),
                          "unsupported dtype");
    ok &= expect_rejected("ambiguous-byte-order.npy",
                          npy_v1(header("|f2", "False", "1,")),
                          "unsupported dtype");

    std::vector<char> narrowed_v2_header = {
        '\x93', 'N', 'U', 'M', 'P', 'Y', 2, 0,
        static_cast<char>(0xff), static_cast<char>(0xff),
        static_cast<char>(0xff), static_cast<char>(0xff)};
    ok &= expect_rejected("v2-u32-header.npy", narrowed_v2_header,
                          "header exceeds limit");

    std::string oversized_header = header("<f4", "False", "1,");
    oversized_header.insert(oversized_header.size() - 1,
                            kHeaderLimit - oversized_header.size() + 2, ' ');
    ok &= expect_rejected("oversized-header.npy",
                          npy(2, 0, oversized_header, std::vector<char>(4)),
                          "header exceeds limit");

    auto oversized_file = npy_v1(header("<f4", "False", "1,"),
                                 std::vector<char>(4));
    oversized_file.resize(kFileLimit + 1, 0);
    ok &= expect_rejected("oversized-file.npy", oversized_file,
                          "file exceeds limit");

    ok &= expect_rejected(
        "rank-limit.npy",
        npy_v1(header("<f2", "False", "1,1,1,1,1,1,1,1,1,"),
               std::vector<char>(2)),
        "rank exceeds limit");
    ok &= expect_rejected("dimension-limit.npy",
                          npy_v1(header("<f2", "False", "4097,"),
                                 std::vector<char>(4097 * 2)),
                          "dimension exceeds limit");
    ok &= expect_rejected("element-limit.npy",
                          npy_v1(header("<f2", "False", "4096,2049,")),
                          "element count exceeds limit");
    ok &= expect_rejected("payload-limit.npy",
                          npy_v1(header("<f8", "False", "4096,2048,")),
                          "payload exceeds limit");
    ok &= expect_rejected("truncated.npy",
                          npy_v1(header("<f4", "False", "2,"),
                                 std::vector<char>(4)),
                          "truncated payload");
    ok &= expect_rejected("trailing.npy",
                          npy_v1(header("<f4", "False", "1,"),
                                 std::vector<char>(8)),
                          "trailing payload");

    struct NonFiniteCase {
        const char* label;
        const char* descr;
        uint64_t bits;
        size_t width;
    };
    const NonFiniteCase non_finite_cases[] = {
        {"f16-positive-infinity.npy", "<f2", 0x7c00u, 2},
        {"f16-negative-infinity.npy", "<f2", 0xfc00u, 2},
        {"f16-nan.npy", "<f2", 0x7e00u, 2},
        {"f32-positive-infinity.npy", "<f4", 0x7f800000u, 4},
        {"f32-negative-infinity.npy", "<f4", 0xff800000u, 4},
        {"f32-nan.npy", "<f4", 0x7fc00000u, 4},
        {"f64-positive-infinity.npy", "<f8", 0x7ff0000000000000ull, 8},
        {"f64-negative-infinity.npy", "<f8", 0xfff0000000000000ull, 8},
        {"f64-nan.npy", "<f8", 0x7ff8000000000000ull, 8},
    };
    for (const NonFiniteCase& test : non_finite_cases) {
        ok &= expect_rejected(
            test.label,
            npy_v1(header(test.descr, "False", "1,"),
                   little_endian(test.bits, test.width)),
            "non-finite payload");
    }

    if (argc != 2) {
        std::fprintf(stderr, "expected path to the shipped spectra NPY\n");
        return 2;
    }
    try {
        const spk::NdArray shipped = spk::load_npy(argv[1]);
        const bool shipped_ok =
            shipped.shape == std::vector<int>({192, 192, 81}) &&
            shipped.data.size() == 2'985'984u;
        if (!shipped_ok) std::fprintf(stderr, "shipped spectra NPY shape/count changed\n");
        ok &= shipped_ok;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "shipped spectra NPY rejected: %s\n", error.what());
        ok = false;
    }

    if (!ok) return 1;
    std::puts("NPY_LUT_HOSTILE_INPUTS: PASS");
    return 0;
}

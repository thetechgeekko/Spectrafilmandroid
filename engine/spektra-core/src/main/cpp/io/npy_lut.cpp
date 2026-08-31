/*
 * Spektrafilm for Android — native engine: .npy / .lut binary loaders.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm. Implements npy_lut.h.
 */
#include "io/npy_lut.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace spk {
namespace {

constexpr size_t kMaxNpyHeaderBytes = 10'000u;
constexpr size_t kMaxNpyRank = 8u;
constexpr size_t kMaxNpyDimension = 4096u;
constexpr size_t kMaxNpyElements = 8u * 1024u * 1024u;
constexpr size_t kMaxNpyPayloadBytes = 32u * 1024u * 1024u;

bool checked_add(size_t a, size_t b, size_t* out) {
    if (b > std::numeric_limits<size_t>::max() - a) return false;
    *out = a + b;
    return true;
}

std::vector<char> read_all(const std::string& path,
                           size_t max_bytes = std::numeric_limits<size_t>::max()) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error(path + ": cannot open for reading");
    std::streamsize size = in.tellg();
    if (size < 0) throw std::runtime_error(path + ": cannot determine size");
    const uintmax_t unsigned_size = static_cast<uintmax_t>(size);
    if (unsigned_size > static_cast<uintmax_t>(max_bytes))
        throw std::runtime_error(path + ": file exceeds limit");
    if (unsigned_size > static_cast<uintmax_t>(std::numeric_limits<size_t>::max()))
        throw std::runtime_error(path + ": file size cannot be represented");
    in.seekg(0, std::ios::beg);
    std::vector<char> buf(static_cast<size_t>(size));
    if (size > 0 && !in.read(buf.data(), size))
        throw std::runtime_error(path + ": read failed");
    return buf;
}

uint16_t rd_u16le(const char* p) {
    return static_cast<uint16_t>((static_cast<uint8_t>(p[0])) |
                                 (static_cast<uint8_t>(p[1]) << 8));
}

uint32_t rd_u32le(const char* p) {
    return static_cast<uint32_t>(static_cast<uint8_t>(p[0])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24);
}

int32_t rd_i32le(const char* p) { return static_cast<int32_t>(rd_u32le(p)); }

float rd_f32le(const char* p) {
    uint32_t bits = rd_u32le(p);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// IEEE-754 half (binary16) little-endian -> double. EXACT (every binary16 value is
// representable in binary64). Builds the result by integer bit-manipulation
// (half -> binary32 bit pattern, then the exact binary32 -> binary64 widening) instead
// of std::ldexp, which the previous version called once PER element — ~3M libm calls
// when loading the 192x192x81 spectra LUT, the bulk of that one-time load. The value is
// bit-identical to the old ldexp path: half -> binary32 is exact (no binary16 overflows
// binary32's range or precision) and binary32 -> binary64 is exact, so the spectra LUT
// parses to the same f64 bits and parity is unchanged. Verified bit-identical over ALL
// 65536 half patterns AND the asset's ~3M elements, and gated end-to-end: the
// engine-parity goldens load the spectra LUT through this path, so any deviation would
// break them. Handles subnormals and inf/nan.
double rd_f16le(const char* p) {
    const uint16_t h = rd_u16le(p);
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;  // -> binary32 bit 31
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t fbits;  // assembled IEEE-754 binary32 bit pattern
    if (exp == 0u) {
        if (mant == 0u) {
            fbits = sign;  // signed zero
        } else {
            // subnormal half -> normalized binary32. Shift the mantissa up until its
            // implicit leading 1 appears, adjusting the exponent (== mant * 2^-24).
            exp = 1u;
            while ((mant & 0x400u) == 0u) { mant <<= 1; --exp; }
            mant &= 0x3FFu;  // drop the now-implicit leading 1
            fbits = sign | ((exp - 15u + 127u) << 23) | (mant << 13);
        }
    } else if (exp == 0x1Fu) {
        // inf (mant==0) or nan -> binary32 inf/nan (payload preserved in the high bits).
        fbits = sign | 0x7F800000u | (mant << 13);
    } else {
        // normal: rebias the 5-bit excess-15 exponent to 8-bit excess-127; widen mant.
        fbits = sign | ((exp - 15u + 127u) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &fbits, sizeof(f));
    return static_cast<double>(f);
}

struct ParsedNpyHeader {
    std::string descr;
    std::string fortran_order;
    std::string shape;
    bool has_descr = false;
    bool has_fortran_order = false;
    bool has_shape = false;
};

void skip_header_space(const std::string& header, size_t end, size_t* cursor) {
    while (*cursor < end &&
           (header[*cursor] == ' ' || header[*cursor] == '\t')) {
        ++*cursor;
    }
}

std::string parse_header_string(const std::string& header, size_t end,
                                size_t* cursor) {
    if (*cursor >= end || header[*cursor] != '\'')
        throw std::runtime_error(".npy: malformed header");
    const size_t start = ++*cursor;
    while (*cursor < end && header[*cursor] != '\'') {
        // Escapes are unnecessary for the fixed NPY keys/dtypes accepted here;
        // rejecting them keeps delimiter handling unambiguous.
        if (header[*cursor] == '\\')
            throw std::runtime_error(".npy: malformed header");
        ++*cursor;
    }
    if (*cursor >= end) throw std::runtime_error(".npy: malformed header");
    const std::string value = header.substr(start, *cursor - start);
    ++*cursor;
    return value;
}

// Parse the deliberately small NPY dictionary grammar instead of searching for
// key-like substrings. This keeps fake/duplicate fields, delayed colons and
// quoted bool/shape values from being interpreted differently from NumPy.
ParsedNpyHeader parse_header_dict(const std::string& header, size_t dict_end) {
    if (dict_end < 2 || header[0] != '{' || header[dict_end - 1] != '}')
        throw std::runtime_error(".npy: malformed header");

    ParsedNpyHeader result;
    size_t cursor = 1;
    skip_header_space(header, dict_end, &cursor);
    while (cursor < dict_end && header[cursor] != '}') {
        const std::string key = parse_header_string(header, dict_end, &cursor);
        skip_header_space(header, dict_end, &cursor);
        if (cursor >= dict_end || header[cursor] != ':')
            throw std::runtime_error(".npy: malformed header");
        ++cursor;
        skip_header_space(header, dict_end, &cursor);

        if (key == "descr") {
            if (result.has_descr)
                throw std::runtime_error(".npy: malformed header");
            result.descr = parse_header_string(header, dict_end, &cursor);
            result.has_descr = true;
        } else if (key == "fortran_order") {
            if (result.has_fortran_order)
                throw std::runtime_error(".npy: malformed header");
            const size_t start = cursor;
            while (cursor < dict_end &&
                   ((header[cursor] >= 'A' && header[cursor] <= 'Z') ||
                    (header[cursor] >= 'a' && header[cursor] <= 'z'))) {
                ++cursor;
            }
            if (cursor == start)
                throw std::runtime_error(".npy: malformed header");
            result.fortran_order = header.substr(start, cursor - start);
            result.has_fortran_order = true;
        } else if (key == "shape") {
            if (result.has_shape || cursor >= dict_end || header[cursor] != '(')
                throw std::runtime_error(".npy: malformed header");
            const size_t start = ++cursor;
            while (cursor < dict_end && header[cursor] != ')') ++cursor;
            if (cursor >= dict_end)
                throw std::runtime_error(".npy: malformed header");
            result.shape = header.substr(start, cursor - start);
            ++cursor;
            result.has_shape = true;
        } else {
            throw std::runtime_error(".npy: malformed header");
        }

        skip_header_space(header, dict_end, &cursor);
        if (cursor >= dict_end)
            throw std::runtime_error(".npy: malformed header");
        if (header[cursor] == ',') {
            ++cursor;
            skip_header_space(header, dict_end, &cursor);
            if (cursor < dict_end && header[cursor] == '}') break;
        } else if (header[cursor] != '}') {
            throw std::runtime_error(".npy: malformed header");
        }
    }

    if (cursor >= dict_end || header[cursor] != '}')
        throw std::runtime_error(".npy: malformed header");
    ++cursor;
    skip_header_space(header, dict_end, &cursor);
    if (cursor != dict_end || !result.has_descr || !result.has_fortran_order ||
        !result.has_shape) {
        throw std::runtime_error(".npy: malformed header");
    }
    return result;
}

}  // namespace

NdArray parse_npy(const char* data, size_t len, const std::string& path) {
    if (len > kMaxNpyFileBytes)
        throw std::runtime_error(path + ": file exceeds limit");
    if (data == nullptr && len != 0)
        throw std::runtime_error(path + ": null .npy input");
    if (len < 8) throw std::runtime_error(path + ": too small for .npy");

    static const char kMagic[6] = {'\x93', 'N', 'U', 'M', 'P', 'Y'};
    if (std::memcmp(data, kMagic, 6) != 0)
        throw std::runtime_error(path + ": not a .npy file (bad magic)");

    const uint8_t major = static_cast<uint8_t>(data[6]);
    const uint8_t minor = static_cast<uint8_t>(data[7]);
    size_t prefix_bytes;
    uint32_t header_len_wire;
    if (major == 1 && minor == 0) {
        prefix_bytes = 10;
        if (len < prefix_bytes) throw std::runtime_error(path + ": truncated header");
        header_len_wire = rd_u16le(data + 8);
    } else if (major == 2 && minor == 0) {
        prefix_bytes = 12;
        if (len < prefix_bytes) throw std::runtime_error(path + ": truncated header");
        header_len_wire = rd_u32le(data + 8);
    } else {
        throw std::runtime_error(path + ": unsupported .npy version");
    }
    if (header_len_wire > kMaxNpyHeaderBytes)
        throw std::runtime_error(path + ": header exceeds limit");
    const size_t header_len = static_cast<size_t>(header_len_wire);
    size_t data_off;
    if (!checked_add(prefix_bytes, header_len, &data_off))
        throw std::runtime_error(path + ": header offset overflow");
    if (data_off > len) throw std::runtime_error(path + ": truncated header");

    std::string hdr(data + prefix_bytes, header_len);
    if (hdr.empty() || hdr.back() != '\n')
        throw std::runtime_error(path + ": malformed header");
    size_t dict_end = hdr.size() - 1;
    while (dict_end > 0 && (hdr[dict_end - 1] == ' ' || hdr[dict_end - 1] == '\t'))
        --dict_end;
    if (dict_end < 2 || hdr.front() != '{' || hdr[dict_end - 1] != '}')
        throw std::runtime_error(path + ": malformed header");

    const ParsedNpyHeader parsed_header = parse_header_dict(hdr, dict_end);
    const std::string& descr = parsed_header.descr;
    const std::string& fortran = parsed_header.fortran_order;
    if (fortran == "True")
        throw std::runtime_error(path + ": fortran_order not supported");
    if (fortran != "False")
        throw std::runtime_error(path + ": invalid fortran_order");

    // Only explicitly little-endian IEEE floating-point arrays are accepted.
    // The bundled LUT is <f2; <f4/<f8 remain supported for upstream parity.
    size_t itemsize;
    int kind;  // 2=f16, 4=f32, 8=f64
    if (descr == "<f2") { itemsize = 2; kind = 2; }
    else if (descr == "<f4") { itemsize = 4; kind = 4; }
    else if (descr == "<f8") { itemsize = 8; kind = 8; }
    else throw std::runtime_error(path + ": unsupported dtype '" + descr + "'");

    // Parse shape tuple, e.g. "192, 192, 81" or "192, 192, 81," (trailing comma).
    const std::string& shp = parsed_header.shape;
    NdArray arr;
    {
        size_t i = 0;
        bool saw_comma = false;
        while (i < shp.size()) {
            while (i < shp.size() && (shp[i] == ' ' || shp[i] == '\t')) ++i;
            if (i >= shp.size()) break;
            if (shp[i] < '0' || shp[i] > '9')
                throw std::runtime_error(path + ": malformed shape");
            size_t dimension = 0;
            while (i < shp.size() && shp[i] >= '0' && shp[i] <= '9') {
                const size_t digit = static_cast<size_t>(shp[i] - '0');
                if (dimension > (kMaxNpyDimension - digit) / 10u)
                    throw std::runtime_error(path + ": dimension exceeds limit");
                dimension = dimension * 10u + digit;
                ++i;
            }
            if (dimension == 0)
                throw std::runtime_error(path + ": zero dimension");
            if (arr.shape.size() >= kMaxNpyRank)
                throw std::runtime_error(path + ": rank exceeds limit");
            arr.shape.push_back(static_cast<int>(dimension));
            while (i < shp.size() && (shp[i] == ' ' || shp[i] == '\t')) ++i;
            if (i >= shp.size()) break;
            if (shp[i] != ',')
                throw std::runtime_error(path + ": malformed shape");
            saw_comma = true;
            ++i;
        }
        if (arr.shape.size() == 1u && !saw_comma)
            throw std::runtime_error(path + ": malformed shape");
    }
    if (arr.shape.empty()) throw std::runtime_error(path + ": empty/scalar shape");

    size_t n = 1;
    for (int dimension : arr.shape) {
        const size_t d = static_cast<size_t>(dimension);
        if (n > kMaxNpyElements / d)
            throw std::runtime_error(path + ": element count exceeds limit");
        n *= d;
    }
    if (n > kMaxNpyPayloadBytes / itemsize)
        throw std::runtime_error(path + ": payload exceeds limit");
    const size_t payload_bytes = n * itemsize;
    const size_t available_bytes = len - data_off;
    if (payload_bytes > available_bytes)
        throw std::runtime_error(path + ": truncated payload");
    if (payload_bytes < available_bytes)
        throw std::runtime_error(path + ": trailing payload");

    // Every hostile-input check above precedes the decoded-double allocation.
    arr.data.resize(n);
    const char* p = data + data_off;

    for (size_t idx = 0; idx < n; ++idx) {
        const char* q = p + idx * itemsize;
        double value;
        if (kind == 2) {
            value = rd_f16le(q);
        } else if (kind == 4) {
            value = static_cast<double>(rd_f32le(q));
        } else {
            uint64_t lo = rd_u32le(q);
            uint64_t hi = rd_u32le(q + 4);
            uint64_t bits = lo | (hi << 32);
            std::memcpy(&value, &bits, sizeof(value));
        }
        if (!std::isfinite(value))
            throw std::runtime_error(path + ": non-finite payload");
        arr.data[idx] = value;
    }
    return arr;
}

NdArray load_npy(const std::string& path) {
    std::vector<char> buf = read_all(path, kMaxNpyFileBytes);
    return parse_npy(buf.data(), buf.size(), path);
}

NdArray load_coeffs_lut(const std::string& path) {
    std::vector<char> buf = read_all(path);
    if (buf.size() < 16) throw std::runtime_error(path + ": too small for .lut");

    // header: 4 x int32 (magic0, magic1, width, height)
    int32_t width = rd_i32le(buf.data() + 8);
    int32_t height = rd_i32le(buf.data() + 12);
    if (width <= 0 || height <= 0)
        throw std::runtime_error(path + ": bad lut dimensions");

    const size_t pixel_bytes = 16;  // 4 x float32
    size_t need = 16 + static_cast<size_t>(width) * height * pixel_bytes;
    if (need > buf.size())
        throw std::runtime_error(path + ": truncated lut payload");

    NdArray arr;
    arr.shape = {width, height, 4};
    arr.data.resize(static_cast<size_t>(width) * height * 4);

    // Pixels iterated as: for j in [0,height): for i in [0,width): read pixel,
    // stored as px[i][j]. Output array indexed [i][j][c] (C-order: i*H*4+j*4+c).
    const char* p = buf.data() + 16;
    for (int j = 0; j < height; ++j) {
        for (int i = 0; i < width; ++i) {
            const char* q = p + (static_cast<size_t>(j) * width + i) * pixel_bytes;
            size_t base = (static_cast<size_t>(i) * height + j) * 4;
            for (int c = 0; c < 4; ++c)
                arr.data[base + c] = static_cast<double>(rd_f32le(q + c * 4));
        }
    }
    return arr;
}

}  // namespace spk

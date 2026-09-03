/* Compare two libraw_device_probe payloads without copying private RAWs off-device. */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

struct Payload {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> values;
};

uint32_t readU32(std::ifstream& input) {
    uint8_t bytes[4]{};
    input.read(reinterpret_cast<char*>(bytes), 4);
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

bool readPayload(const char* path, Payload& payload) {
    std::ifstream input(path, std::ios::binary);
    char magic[8]{};
    input.read(magic, 8);
    if (!input || std::memcmp(magic, "SFRF32\0\1", 8) != 0) return false;
    payload.width = readU32(input);
    payload.height = readU32(input);
    const uint32_t channels = readU32(input);
    const uint32_t count = readU32(input);
    if (!input || channels != 3 || count != payload.width * payload.height * 3u)
        return false;
    payload.values.resize(count);
    input.read(reinterpret_cast<char*>(payload.values.data()),
               static_cast<std::streamsize>(count * sizeof(float)));
    return static_cast<bool>(input);
}

uint32_t orderedFloatBits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x80000000u) ? ~bits : (bits | 0x80000000u);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    Payload lhs, rhs;
    if (!readPayload(argv[1], lhs) || !readPayload(argv[2], rhs)) return 3;
    if (lhs.width != rhs.width || lhs.height != rhs.height ||
        lhs.values.size() != rhs.values.size()) {
        std::cout << "shape_equal=0 lhs=" << lhs.width << 'x' << lhs.height
                  << " rhs=" << rhs.width << 'x' << rhs.height << '\n';
        return 1;
    }

    uint64_t different = 0;
    uint64_t nonFinite = 0;
    uint64_t maxUlp = 0;
    double sumAbs = 0.0;
    double maxAbs = 0.0;
    size_t maxIndex = 0;
    for (size_t i = 0; i < lhs.values.size(); ++i) {
        const float a = lhs.values[i];
        const float b = rhs.values[i];
        if (std::memcmp(&a, &b, sizeof(float)) == 0) continue;
        ++different;
        if (!std::isfinite(a) || !std::isfinite(b)) {
            ++nonFinite;
            continue;
        }
        const double absDelta = std::abs(static_cast<double>(a) - b);
        sumAbs += absDelta;
        if (absDelta > maxAbs) {
            maxAbs = absDelta;
            maxIndex = i;
        }
        const uint32_t oa = orderedFloatBits(a);
        const uint32_t ob = orderedFloatBits(b);
        maxUlp = std::max<uint64_t>(maxUlp, oa > ob ? oa - ob : ob - oa);
    }

    std::cout << std::setprecision(12)
              << "shape_equal=1 values=" << lhs.values.size()
              << " different=" << different
              << " different_fraction="
              << (static_cast<double>(different) / lhs.values.size())
              << " max_abs=" << maxAbs
              << " mean_abs_changed=" << (different ? sumAbs / different : 0.0)
              << " max_ulp=" << maxUlp
              << " non_finite=" << nonFinite
              << " max_index=" << maxIndex
              << " lhs_at_max=" << lhs.values[maxIndex]
              << " rhs_at_max=" << rhs.values[maxIndex] << '\n';
    return different == 0 ? 0 : 1;
}

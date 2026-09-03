/*
 * Device-side decoded-byte probe for LibRaw upgrade/parity evidence.
 *
 * Link this tiny executable against a candidate libsfraw.so, run it under
 * /data/local/tmp, then hash the emitted .sfraw-f32 files on the host. Keeping
 * hashing outside the target avoids introducing a second crypto implementation.
 */
#include "raw_decoder.h"

#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

bool readFile(const char* path, std::vector<uint8_t>& bytes) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const std::streamoff length = input.tellg();
    if (length <= 0) return false;
    bytes.resize(static_cast<size_t>(length));
    input.seekg(0, std::ios::beg);
    return static_cast<bool>(
        input.read(reinterpret_cast<char*>(bytes.data()), length));
}

void writeU32(std::ofstream& output, uint32_t value) {
    const uint8_t littleEndian[4] = {
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 24),
    };
    output.write(reinterpret_cast<const char*>(littleEndian), 4);
}

bool writeResult(const std::string& path,
                 const spectrafilm::DecodeResult& result) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write("SFRF32\0\1", 8);
    writeU32(output, static_cast<uint32_t>(result.width));
    writeU32(output, static_cast<uint32_t>(result.height));
    writeU32(output, 3);
    writeU32(output, static_cast<uint32_t>(result.rgb.size()));
    output.write(reinterpret_cast<const char*>(result.rgb.data()),
                 static_cast<std::streamsize>(result.rgb.size() * sizeof(float)));
    return static_cast<bool>(output);
}

class OwnedFd final {
 public:
    explicit OwnedFd(const char* path)
        : fd_(::open(path, O_RDONLY | O_CLOEXEC)) {}
    ~OwnedFd() {
        if (fd_ >= 0) ::close(fd_);
    }
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    int get() const noexcept { return fd_; }

 private:
    int fd_;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 7 && argc != 8 && argc != 9) {
        std::cerr << "usage: libraw_device_probe INPUT OUTPUT_PREFIX REPEATS "
                     "[as_shot|daylight|tungsten|custom TEMPERATURE_K TINT "
                     "[MAX_LONG_EDGE [buffer|fd]]]\n";
        return 2;
    }

    int repeats = 0;
    try {
        repeats = std::stoi(argv[3]);
    } catch (...) {
        return 2;
    }
    if (repeats < 1 || repeats > 20) return 2;

    spectrafilm::DecodeOptions options;
    options.whiteBalance = spectrafilm::WhiteBalanceMode::AsShot;
    if (argc >= 7) {
        const std::string mode = argv[4];
        if (mode == "as_shot") {
            options.whiteBalance = spectrafilm::WhiteBalanceMode::AsShot;
        } else if (mode == "daylight") {
            options.whiteBalance = spectrafilm::WhiteBalanceMode::Daylight;
        } else if (mode == "tungsten") {
            options.whiteBalance = spectrafilm::WhiteBalanceMode::Tungsten;
        } else if (mode == "custom") {
            options.whiteBalance = spectrafilm::WhiteBalanceMode::Custom;
        } else {
            std::cerr << "unsupported white-balance mode: " << mode << "\n";
            return 2;
        }
        try {
            options.temperatureK = std::stod(argv[5]);
            options.tint = std::stod(argv[6]);
        } catch (...) {
            std::cerr << "invalid temperature/tint\n";
            return 2;
        }
    }
    options.halfSize = false;
    options.maxLongEdge = 0;
    if (argc == 8) {
        try {
            options.maxLongEdge = std::stoi(argv[7]);
        } catch (...) {
            return 2;
        }
        if (options.maxLongEdge < 0 || options.maxLongEdge > 16384) return 2;
    }
    if (argc == 9) {
        try {
            options.maxLongEdge = std::stoi(argv[7]);
        } catch (...) {
            return 2;
        }
        if (options.maxLongEdge < 0 || options.maxLongEdge > 16384) return 2;
    }
    const std::string inputMode = argc == 9 ? argv[8] : "buffer";
    if (inputMode != "buffer" && inputMode != "fd") {
        std::cerr << "unsupported input mode: " << inputMode << "\n";
        return 2;
    }

    std::vector<uint8_t> input;
    if (inputMode == "buffer" && !readFile(argv[1], input)) {
        std::cerr << "failed to read " << argv[1] << "\n";
        return 3;
    }
    OwnedFd inputFd(argv[1]);
    if (inputMode == "fd" && inputFd.get() < 0) {
        std::cerr << "failed to open " << argv[1] << "\n";
        return 3;
    }

    for (int iteration = 0; iteration < repeats; ++iteration) {
        if (inputMode == "fd" && ::lseek(inputFd.get(), 0, SEEK_SET) < 0) {
            std::cerr << "failed to rewind " << argv[1] << "\n";
            return 3;
        }
        const auto decodeAt = std::chrono::steady_clock::now();
        spectrafilm::DecodeResult result = inputMode == "fd"
            ? spectrafilm::decodeFromFd(inputFd.get(), options)
            : spectrafilm::decodeFromBuffer(input.data(), input.size(), options);
        const double decodeMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - decodeAt).count();
        if (!result.ok) {
            std::cerr << "decode failed: status=" << result.status
                      << " libraw=" << result.librawCode
                      << " error=" << result.error << "\n";
            return 4;
        }
        const std::string outputPath =
            std::string(argv[2]) + "." + std::to_string(iteration) + ".sfraw-f32";
        const auto writeAt = std::chrono::steady_clock::now();
        if (!writeResult(outputPath, result)) {
            std::cerr << "failed to write " << outputPath << "\n";
            return 5;
        }
        const double writeMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - writeAt).count();
        std::cout << "iteration=" << iteration
                  << " wb_mode=" << (argc >= 7 ? argv[4] : "as_shot")
                  << " temperature_k=" << options.temperatureK
                  << " tint=" << options.tint
                  << " max_long_edge=" << options.maxLongEdge
                  << " input_mode=" << inputMode
                  << " width=" << result.width
                  << " height=" << result.height
                  << " floats=" << result.rgb.size()
                  << " decode_ms=" << decodeMs
                  << " write_ms=" << writeMs
                  << " output=" << outputPath << "\n";
    }
    return 0;
}

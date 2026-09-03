/*
 * Spektrafilm — libFuzzer harness for the DNG compression sniffer (GPLv3).
 *
 * The sniffer (dngsniff::compressionOf / classifyUnpackFailure) is the part of the
 * RAW decoder that parses FULLY UNTRUSTED bytes (a malformed DNG/TIFF header + IFD
 * chain) after a LibRaw unpack failure, so it remains a high-value memory-safety
 * target in the fallback path. This harness drives both entry points on
 * fuzzer-mutated input.
 *
 * Build (host, no Android, no LibRaw — the TU compiles with SFRAW_HAVE_LIBRAW=0):
 *   clang++ -std=c++17 -O1 -g -fsanitize=fuzzer,address,undefined \
 *     -I ../main/cpp -include ../main/cpp/raw_decoder.cpp \
 *     fuzz_dng_sniffer.cpp -o /tmp/fuzz_dng_sniffer
 *   /tmp/fuzz_dng_sniffer -max_total_time=60 -rss_limit_mb=2048
 *
 * Note: -include pulls in raw_decoder.cpp (its file-scope helpers are in anonymous
 * / dngsniff namespaces, so there is no ODR clash with this single-TU build).
 */
#include <cstddef>
#include <cstdint>

namespace spectrafilm {
namespace dngsniff {
int compressionOf(const uint8_t* data, size_t len, bool* isDng);
int classifyUnpackFailure(const uint8_t* data, size_t len);
}  // namespace dngsniff
}  // namespace spectrafilm

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    bool isDng = false;
    // Exercise the IFD walker / compression detector on arbitrary bytes.
    volatile int c = spectrafilm::dngsniff::compressionOf(data, size, &isDng);
    (void)c;
    volatile int s = spectrafilm::dngsniff::classifyUnpackFailure(data, size);
    (void)s;
    return 0;
}

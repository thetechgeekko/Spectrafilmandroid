/*
 * Spektrafilm for Android — bounded libFuzzer entry point for native JSON seams.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "profiles/json_min.h"
#include "profiles/profile.h"
#include "runtime/print_digest.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr || size > spk::json::kMaxInputBytes + 1) return 0;
    const std::string input(reinterpret_cast<const char*>(data), size);

    try {
        (void)spk::json::parse(input);
    } catch (...) {
    }
    try {
        (void)spk::load_profile_string(input);
    } catch (...) {
    }

    const double sentinels[3] = {101.25, -202.5, 303.75};
    double cc[3];
    std::memcpy(cc, sentinels, sizeof(cc));
    const bool found = spk::resolve_neutral_cc_string(
        input, "kodak_portra_endura", "TH-KG3", "kodak_portra_400", cc);
    if (!found && std::memcmp(cc, sentinels, sizeof(cc)) != 0) __builtin_trap();
    if (found && (!std::isfinite(cc[0]) || !std::isfinite(cc[1]) ||
                  !std::isfinite(cc[2])))
        __builtin_trap();
    return 0;
}

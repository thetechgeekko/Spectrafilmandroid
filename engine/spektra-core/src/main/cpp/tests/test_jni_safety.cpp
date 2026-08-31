#include "../jni_safety.h"

#include <cassert>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

int main() {
    using spk::jni::AllocationRegistry;
    using spk::jni::copy_bytes_cancellable;
    using spk::jni::checked_float_buffer_range;
    using spk::jni::checked_rgb_f32_bytes;

    std::uint64_t bytes = 0;
    assert(checked_rgb_f32_bytes(1, 1, &bytes) && bytes == 12);
    assert(checked_rgb_f32_bytes(100, 200, &bytes) && bytes == 240000);
    assert(!checked_rgb_f32_bytes(0, 1, &bytes));
    assert(!checked_rgb_f32_bytes(-1, 1, &bytes));
    assert(!checked_rgb_f32_bytes(
        std::numeric_limits<std::int32_t>::max(),
        std::numeric_limits<std::int32_t>::max(), &bytes));

    alignas(float) std::uint8_t logical[64] = {};
    std::uintptr_t address = 0;
    const auto base = reinterpret_cast<std::uintptr_t>(logical);
    assert(checked_float_buffer_range(64, 4, 52, 48, base, &address));
    assert(address == base + 4);
    assert(!checked_float_buffer_range(64, 2, 52, 48, base, &address));
    assert(!checked_float_buffer_range(64, 4, 51, 48, base, &address));
    assert(!checked_float_buffer_range(64, 8, 65, 48, base, &address));
    assert(!checked_float_buffer_range(64, -1, 64, 48, base, &address));

    // A cancellation that arrives during a large JNI result transfer must stop
    // before a successful result can be published. The caller discards the
    // partially copied destination.
    constexpr std::size_t kChunk = 1024;
    std::vector<std::uint8_t> source(kChunk * 2 + 17);
    for (std::size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<std::uint8_t>(i % 251U);
    }
    std::vector<std::uint8_t> destination(source.size(), 0U);
    struct CopyCancellation {
        int polls = 0;
        int cancel_on = 0;
    } copy_cancellation{0, 2};
    auto cancel_copy = [](void* opaque) noexcept -> int {
        auto* state = static_cast<CopyCancellation*>(opaque);
        return ++state->polls >= state->cancel_on ? 1 : 0;
    };
    assert(!copy_bytes_cancellable(destination.data(), source.data(), source.size(),
                                   cancel_copy, &copy_cancellation, kChunk));
    assert(copy_cancellation.polls == 2);
    assert(std::equal(source.begin(), source.begin() + kChunk,
                      destination.begin()));
    assert(std::all_of(destination.begin() + kChunk, destination.end(),
                       [](std::uint8_t value) { return value == 0U; }));

    copy_cancellation = {0, std::numeric_limits<int>::max()};
    assert(copy_bytes_cancellable(destination.data(), source.data(), source.size(),
                                  cancel_copy, &copy_cancellation, kChunk));
    assert(destination == source);

    AllocationRegistry registry;
    alignas(float) std::uint8_t allocation[64] = {};
    std::uint64_t token = 0;
    assert(registry.add(allocation, sizeof(allocation), &token));
    assert(token != 0);
    assert(registry.size() == 1);

    std::size_t registered_size = 0;
    assert(!registry.take(allocation + sizeof(float), sizeof(allocation) - sizeof(float), token,
                          &registered_size));
    assert(registry.size() == 1);
    assert(!registry.take(allocation, sizeof(allocation) - 1, token, &registered_size));
    assert(registry.size() == 1);
    assert(!registry.take(allocation, sizeof(allocation), token + 1, &registered_size));
    assert(registry.size() == 1);
    assert(registry.take(allocation, sizeof(allocation), token, &registered_size));
    assert(registered_size == sizeof(allocation));
    assert(registry.size() == 0);
    assert(!registry.take(allocation, sizeof(allocation), token, &registered_size));

    // Simulate malloc reusing the same address: a stale owner token from the
    // prior allocation must not release the new allocation (ABA resistance).
    std::uint64_t reused_token = 0;
    assert(registry.add(allocation, sizeof(allocation), &reused_token));
    assert(reused_token != token);
    assert(!registry.take(allocation, sizeof(allocation), token));
    assert(registry.take(allocation, sizeof(allocation), reused_token));

    AllocationRegistry raced;
    std::uint64_t raced_token = 0;
    assert(raced.add(allocation, sizeof(allocation), &raced_token));
    std::atomic<int> winners{0};
    std::vector<std::thread> closers;
    for (int i = 0; i < 16; ++i) {
        closers.emplace_back([&] {
            if (raced.take(allocation, sizeof(allocation), raced_token)) ++winners;
        });
    }
    for (auto& closer : closers) closer.join();
    assert(winners.load() == 1);
    assert(raced.size() == 0);
}

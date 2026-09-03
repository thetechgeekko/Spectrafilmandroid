#include "../jni_safety.h"
#include "../runtime/memory_budget.h"

#include <cassert>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

int main() {
    using spk::jni::AllocationRegistry;
    using spk::jni::ExternalReservationRegistry;
    using spk::jni::copy_bytes_cancellable;
    using spk::jni::checked_float_buffer_range;
    using spk::jni::checked_rgb_f32_bytes;
    using spk::memory::MemoryBudget;
    using spk::memory::MemoryDomain;
    using spk::memory::MemoryStage;

    static_assert(static_cast<std::uint8_t>(MemoryDomain::Unknown) == 0);
    static_assert(static_cast<std::uint8_t>(MemoryDomain::Jvm) == 8);
    static_assert(static_cast<std::uint8_t>(MemoryStage::Unknown) == 0);
    static_assert(static_cast<std::uint8_t>(MemoryStage::Gpu) == 11);

    // A reservation is admitted atomically against the process ceiling, and
    // diagnostics retain exact current/high-water/denial totals by domain and
    // stage even after the RAII owner releases its bytes.
    MemoryBudget budget(128);
    auto result_reservation = budget.try_reserve(
        64, MemoryDomain::JniOwned, MemoryStage::JniSimResult);
    assert(result_reservation);
    auto budget_snapshot = budget.snapshot();
    assert(budget_snapshot.limit_bytes == 128);
    assert(budget_snapshot.total.current_bytes == 64);
    assert(budget_snapshot.total.peak_bytes == 64);
    assert(budget_snapshot.domains[static_cast<std::size_t>(MemoryDomain::JniOwned)]
               .current_bytes == 64);
    assert(budget_snapshot.stages[static_cast<std::size_t>(MemoryStage::JniSimResult)]
               .peak_bytes == 64);

    auto denied_reservation = budget.try_reserve(
        65, MemoryDomain::NativeScratch, MemoryStage::Spatial);
    assert(!denied_reservation);
    budget_snapshot = budget.snapshot();
    assert(budget_snapshot.total.current_bytes == 64);
    assert(budget_snapshot.total.denial_count == 1);
    assert(budget_snapshot.total.denied_bytes == 65);
    assert(budget_snapshot.domains[static_cast<std::size_t>(MemoryDomain::NativeScratch)]
               .denial_count == 1);
    assert(budget_snapshot.stages[static_cast<std::size_t>(MemoryStage::Spatial)]
               .denied_bytes == 65);

    auto direct_reservation = budget.try_reserve(
        32, MemoryDomain::JniOwned, MemoryStage::JniDirectBuffer);
    assert(direct_reservation);
    budget_snapshot = budget.snapshot();
    assert(budget_snapshot.total.current_bytes == 96);
    assert(budget_snapshot.total.peak_bytes == 96);
    result_reservation.reset();
    assert(budget.snapshot().total.current_bytes == 32);

    // Move transfer must preserve one exact release; a moved-from reservation
    // cannot decrement the counters a second time.
    auto moved_reservation = std::move(direct_reservation);
    assert(!direct_reservation);
    assert(moved_reservation);
    moved_reservation.reset();
    assert(budget.snapshot().total.current_bytes == 0);

    const std::string snapshot_json =
        spk::memory::memory_budget_snapshot_json(budget.snapshot());
    assert(snapshot_json ==
           spk::memory::memory_budget_snapshot_json(budget.snapshot()));
    assert(snapshot_json.find(
               "{\"schema\":\"spk.memory_budget.v1\",\"limit_bytes\":128,") == 0);
    assert(snapshot_json.find(
               "\"total\":{\"current_bytes\":0,\"peak_bytes\":96,") !=
           std::string::npos);
    std::size_t ordered_name = 0;
    for (const char* name : {"unknown", "jni_owned", "native_image",
                             "native_scratch", "cache", "gpu_host",
                             "gpu_device", "writer", "jvm"}) {
        ordered_name = snapshot_json.find(
            std::string("\"name\":\"") + name + '"', ordered_name);
        assert(ordered_name != std::string::npos);
        ++ordered_name;
    }
    for (const char* name : {"unknown", "jni_sim_result", "jni_direct_buffer",
                             "decode", "filming", "scanning", "printing",
                             "spatial", "grain", "lut", "writer", "gpu"}) {
        ordered_name = snapshot_json.find(
            std::string("\"name\":\"") + name + '"', ordered_name);
        assert(ordered_name != std::string::npos);
        ++ordered_name;
    }
    assert(snapshot_json.size() >= 2 &&
           snapshot_json.compare(snapshot_json.size() - 2, 2, "]}") == 0);

    MemoryBudget overflow_budget(std::numeric_limits<std::uint64_t>::max());
    auto nearly_all = overflow_budget.try_reserve(
        std::numeric_limits<std::uint64_t>::max() - 7,
        MemoryDomain::NativeScratch, MemoryStage::Spatial);
    assert(nearly_all);
    assert(!overflow_budget.try_reserve(
        8, MemoryDomain::NativeScratch, MemoryStage::Spatial));
    assert(overflow_budget.snapshot().total.current_bytes ==
           std::numeric_limits<std::uint64_t>::max() - 7);
    nearly_all.reset();

    MemoryBudget invalid_tag_budget(1);
    auto sanitized_tags = invalid_tag_budget.try_reserve(
        1, static_cast<MemoryDomain>(255), static_cast<MemoryStage>(255));
    assert(sanitized_tags);
    auto invalid_tag_snapshot = invalid_tag_budget.snapshot();
    assert(invalid_tag_snapshot.domains[0].current_bytes == 1);
    assert(invalid_tag_snapshot.stages[0].current_bytes == 1);
    sanitized_tags.reset();

    // Concurrent admissions cannot oversubscribe the ceiling. Reservations are
    // retained until all contenders finish, making four 64-byte winners exact.
    MemoryBudget concurrent_budget(256);
    std::vector<spk::memory::MemoryReservation> held(16);
    std::vector<std::thread> reservers;
    std::atomic<bool> start_reservers{false};
    for (std::size_t i = 0; i < held.size(); ++i) {
        reservers.emplace_back([&, i] {
            while (!start_reservers.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            held[i] = concurrent_budget.try_reserve(
                64, MemoryDomain::NativeScratch, MemoryStage::Spatial);
        });
    }
    start_reservers.store(true, std::memory_order_release);
    for (auto& reserver : reservers) reserver.join();
    const auto admitted = std::count_if(
        held.begin(), held.end(),
        [](const spk::memory::MemoryReservation& reservation) {
            return static_cast<bool>(reservation);
        });
    const auto concurrent_snapshot = concurrent_budget.snapshot();
    assert(admitted == 4);
    assert(concurrent_snapshot.total.current_bytes == 256);
    assert(concurrent_snapshot.total.peak_bytes == 256);
    assert(concurrent_snapshot.total.denial_count == 12);
    for (auto& reservation : held) reservation.reset();
    assert(concurrent_budget.snapshot().total.current_bytes == 0);

    // Externally-owned JVM bytes participate in the same ceiling without giving
    // native code pointer/free authority. The opaque token is exact-once.
    MemoryBudget external_budget(128);
    ExternalReservationRegistry external_registry(
        MemoryDomain::Jvm, external_budget);
    assert(external_registry.reserve(0, MemoryStage::Decode) == 0);
    assert(!external_registry.release(0));
    const std::uint64_t external_token = external_registry.reserve(
        96, MemoryStage::Decode);
    assert(external_token != 0);
    assert(external_registry.reserve(33, MemoryStage::Decode) == 0);
    const auto external_snapshot = external_budget.snapshot();
    assert(external_snapshot.total.current_bytes == 96);
    assert(external_snapshot.total.denial_count == 1);
    assert(external_snapshot.domains[static_cast<std::size_t>(MemoryDomain::Jvm)]
               .current_bytes == 96);
    assert(external_snapshot.stages[static_cast<std::size_t>(MemoryStage::Decode)]
               .peak_bytes == 96);
    assert(external_registry.release(external_token));
    assert(!external_registry.release(external_token));
    assert(external_budget.snapshot().total.current_bytes == 0);

    const std::uint64_t raced_external_token = external_registry.reserve(
        64, MemoryStage::Decode);
    assert(raced_external_token != 0);
    std::atomic<int> external_release_winners{0};
    std::vector<std::thread> external_releasers;
    for (int i = 0; i < 16; ++i) {
        external_releasers.emplace_back([&] {
            if (external_registry.release(raced_external_token)) {
                ++external_release_winners;
            }
        });
    }
    for (auto& releaser : external_releasers) releaser.join();
    assert(external_release_winners.load() == 1);
    assert(external_registry.size() == 0);
    assert(external_budget.snapshot().total.current_bytes == 0);

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

    MemoryBudget registry_budget(64);
    AllocationRegistry registry(registry_budget);
    alignas(float) std::uint8_t allocation[64] = {};
    std::uint64_t token = 0;
    auto registry_reservation = registry.reserve(
        sizeof(allocation), MemoryStage::JniSimResult);
    assert(registry_reservation);
    assert(registry_budget.snapshot().total.current_bytes == sizeof(allocation));
    assert(registry.add_reserved(allocation, sizeof(allocation), &token,
                                 registry_reservation));
    assert(!registry_reservation);
    // Registering transfers the existing reservation; it must not count the
    // same JNI allocation a second time.
    assert(registry_budget.snapshot().total.current_bytes == sizeof(allocation));
    assert(token != 0);
    assert(registry.size() == 1);

    assert(!registry.take(allocation + sizeof(float),
                          sizeof(allocation) - sizeof(float), token));
    assert(registry.size() == 1);
    assert(!registry.take(allocation, sizeof(allocation) - 1, token));
    assert(registry.size() == 1);
    assert(!registry.take(allocation, sizeof(allocation), token + 1));
    assert(registry.size() == 1);
    auto taken = registry.take(allocation, sizeof(allocation), token);
    assert(taken);
    assert(taken.base() == allocation);
    assert(taken.size() == sizeof(allocation));
    assert(registry.size() == 0);
    // Removal transfers free authority, but admission remains live across the
    // caller's free window so no competing allocation can oversubscribe it.
    assert(registry_budget.snapshot().total.current_bytes == sizeof(allocation));
    assert(!registry.reserve(sizeof(allocation), MemoryStage::JniDirectBuffer));
    taken = {};
    assert(registry_budget.snapshot().total.current_bytes == 0);
    assert(registry_budget.snapshot()
               .stages[static_cast<std::size_t>(MemoryStage::JniSimResult)]
               .peak_bytes == sizeof(allocation));
    assert(!registry.take(allocation, sizeof(allocation), token));

    // Simulate malloc reusing the same address: a stale owner token from the
    // prior allocation must not release the new allocation (ABA resistance).
    std::uint64_t reused_token = 0;
    assert(registry.add(allocation, sizeof(allocation), &reused_token));
    assert(reused_token != token);
    assert(!registry.take(allocation, sizeof(allocation), token));
    auto reused = registry.take(allocation, sizeof(allocation), reused_token);
    assert(reused);
    assert(registry_budget.snapshot().total.current_bytes == sizeof(allocation));
    reused = {};

    MemoryBudget raced_budget(sizeof(allocation));
    AllocationRegistry raced(raced_budget);
    std::uint64_t raced_token = 0;
    assert(raced.add(allocation, sizeof(allocation), &raced_token));
    std::atomic<int> winners{0};
    std::vector<std::thread> closers;
    for (int i = 0; i < 16; ++i) {
        closers.emplace_back([&] {
            if (auto owned = raced.take(
                    allocation, sizeof(allocation), raced_token)) {
                ++winners;
            }
        });
    }
    for (auto& closer : closers) closer.join();
    assert(winners.load() == 1);
    assert(raced.size() == 0);
    assert(raced_budget.snapshot().total.current_bytes == 0);
}

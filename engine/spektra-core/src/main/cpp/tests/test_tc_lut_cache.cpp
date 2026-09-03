/*
 * Spektrafilm for Android — bounded filming tc_lut cache regressions.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
#include <cassert>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <mutex>
#include <new>
#include <vector>

#include "runtime/memory_budget.h"
#include "runtime/tc_lut_cache.h"

namespace {

spk::NdArray make_lut(double seed, std::size_t cells = 12) {
    spk::NdArray result;
    result.shape = {2, 2, 3};
    result.data.resize(cells);
    for (std::size_t i = 0; i < cells; ++i) {
        result.data[i] = seed + static_cast<double>(i) * 0.25;
    }
    return result;
}

struct PayloadTeardownBarrier {
    std::mutex mutex;
    std::condition_variable cv;
    bool payload_deallocated = false;
    bool allow_budget_release = false;

    static void wait_before_budget_release(void* opaque) noexcept {
        auto& barrier = *static_cast<PayloadTeardownBarrier*>(opaque);
        std::unique_lock<std::mutex> lock(barrier.mutex);
        barrier.payload_deallocated = true;
        barrier.cv.notify_all();
        barrier.cv.wait(lock, [&] { return barrier.allow_budget_release; });
    }
};

}  // namespace

int main() {
    {
        spk::memory::MemoryBudget global_budget(1024u * 1024u);
        spk::TcLutCacheConfig config;
        config.dynamic_byte_budget = 64u * 1024u;
        config.pinned_byte_budget = 64u * 1024u;
        config.memory_budget = &global_budget;
        spk::TcLutCache cache(config);

        std::uint64_t builds = 0;
        auto first = cache.get_or_build("stock-default", true, [&] {
            ++builds;
            return make_lut(1.0);
        });
        auto second = cache.get_or_build("stock-default", true, [&] {
            ++builds;
            return make_lut(9.0);
        });

        const auto stats = cache.stats();
        assert(first);
        assert(second);
        assert(first.get() == second.get());
        assert(builds == 1);
        assert(stats.misses == 1);
        assert(stats.hits == 1);
        assert(stats.pinned_entries == 1);
        assert(stats.dynamic_entries == 0);
        assert(stats.cache_held_bytes == stats.pinned_bytes);
        assert(stats.cache_held_bytes > first->data.size() * sizeof(double));

        // Classification is a key invariant: engine default keys are bare and
        // parameterized keys are composite. If a future caller violates that,
        // the first admission remains authoritative and the conflict is visible.
        auto conflict = cache.get_or_build("stock-default", false, [&] {
            ++builds;
            return make_lut(99.0);
        });
        const auto mixed = cache.stats();
        assert(conflict.get() == first.get());
        assert(builds == 1);
        assert(mixed.classification_conflicts == 1);
        assert(mixed.pinned_entries == 1);
        assert(mixed.dynamic_entries == 0);
    }

    // The dynamic side is byte-budgeted and true LRU: touching A protects it,
    // so C evicts B. Rebuilding an evicted key must reproduce every byte.
    {
        spk::memory::MemoryBudget probe_budget(1024u * 1024u);
        spk::TcLutCacheConfig probe_config;
        probe_config.dynamic_byte_budget = 1024u * 1024u;
        probe_config.memory_budget = &probe_budget;
        spk::TcLutCache probe(probe_config);
        auto ignored = probe.get_or_build("slider-00", false,
                                          [] { return make_lut(2.0); });
        const std::size_t one_entry_bytes = probe.stats().dynamic_bytes;
        assert(one_entry_bytes > 0);

        spk::memory::MemoryBudget global_budget(1024u * 1024u);
        spk::TcLutCacheConfig config;
        config.dynamic_byte_budget = one_entry_bytes * 2;
        config.pinned_byte_budget = one_entry_bytes * 2;
        config.memory_budget = &global_budget;
        spk::TcLutCache cache(config);

        std::uint64_t a_builds = 0;
        std::uint64_t b_builds = 0;
        auto a0 = cache.get_or_build("slider-00", false, [&] {
            ++a_builds;
            return make_lut(10.0);
        });
        const std::vector<double> a_bytes = a0->data;
        cache.get_or_build("slider-01", false, [&] {
            ++b_builds;
            return make_lut(20.0);
        });
        auto a_hit = cache.get_or_build("slider-00", false, [&] {
            ++a_builds;
            return make_lut(99.0);
        });
        cache.get_or_build("slider-02", false,
                           [] { return make_lut(30.0); });
        assert(cache.stats().evictions == 1);
        assert(cache.stats().dynamic_entries == 2);
        assert(cache.stats().dynamic_bytes <= config.dynamic_byte_budget);
        assert(a_hit.get() == a0.get());

        // B was least-recently-used. Its rebuild evicts A, then asking for A
        // rebuilds it byte-for-byte while the old lease remains valid.
        cache.get_or_build("slider-01", false, [&] {
            ++b_builds;
            return make_lut(20.0);
        });
        auto a1 = cache.get_or_build("slider-00", false, [&] {
            ++a_builds;
            return make_lut(10.0);
        });
        assert(a_builds == 2);
        assert(b_builds == 2);
        assert(a1.get() != a0.get());
        assert(a1->data.size() == a_bytes.size());
        assert(std::memcmp(a1->data.data(), a_bytes.data(),
                           a_bytes.size() * sizeof(double)) == 0);

        for (int i = 3; i < 259; ++i) {
            const std::string key = "slider-" +
                (i < 10 ? std::string("0") : std::string()) +
                std::to_string(i);
            cache.get_or_build(key, false,
                               [i] { return make_lut(100.0 + i); });
            const auto sweep = cache.stats();
            assert(sweep.dynamic_entries <= 2);
            assert(sweep.dynamic_bytes <= config.dynamic_byte_budget);
        }
        assert(cache.stats().evictions >= 256);
    }

    // Accounting includes allocator-requested owner/control storage, the key,
    // shape capacity, and value capacity — not just the double payload.
    {
        spk::memory::MemoryBudget global_budget(1024u * 1024u);
        spk::TcLutCacheConfig config;
        config.dynamic_byte_budget = 1024u * 1024u;
        config.memory_budget = &global_budget;
        spk::TcLutCache short_key_cache(config);
        short_key_cache.get_or_build("k", false, [] { return make_lut(1.0); });
        const std::size_t short_charge = short_key_cache.stats().dynamic_bytes;

        spk::TcLutCache long_key_cache(config);
        const std::string long_key(1024, 'x');
        long_key_cache.get_or_build(long_key, false,
                                    [] { return make_lut(1.0); });
        const std::size_t long_charge = long_key_cache.stats().dynamic_bytes;
        assert(short_charge > 12 * sizeof(double));
        assert(long_charge >= short_charge + long_key.size());
        assert(global_budget.snapshot().domains[static_cast<std::size_t>(
                   spk::memory::MemoryDomain::Cache)].current_bytes ==
               short_charge + long_charge);
    }

    // All 28 bundled/default keys remain pinned while continuous parameter keys
    // churn only the dynamic LRU. A 29th pinned key is served but deliberately
    // not admitted, so even unexpected assets cannot grow the pinned set.
    {
        spk::memory::MemoryBudget global_budget(16u * 1024u * 1024u);
        spk::TcLutCacheConfig config;
        config.dynamic_byte_budget = 2048;
        config.pinned_byte_budget = 64u * 1024u;
        config.memory_budget = &global_budget;
        spk::TcLutCache cache(config);
        std::vector<std::uint64_t> builds(29, 0);
        for (int i = 0; i < 28; ++i) {
            const std::string key = "stock-" + std::to_string(100 + i);
            cache.get_or_build(key, true, [&, i] {
                ++builds[static_cast<std::size_t>(i)];
                return make_lut(i);
            });
        }
        assert(cache.stats().pinned_entries == 28);

        for (int i = 0; i < 300; ++i) {
            cache.get_or_build("blur-" + std::to_string(i), false,
                               [i] { return make_lut(1000.0 + i); });
        }
        assert(cache.stats().pinned_entries == 28);
        assert(cache.stats().dynamic_bytes <= config.dynamic_byte_budget);
        for (int i = 0; i < 28; ++i) {
            const std::string key = "stock-" + std::to_string(100 + i);
            cache.get_or_build(key, true, [&, i] {
                ++builds[static_cast<std::size_t>(i)];
                return make_lut(9999.0);
            });
            assert(builds[static_cast<std::size_t>(i)] == 1);
        }

        for (int attempt = 0; attempt < 2; ++attempt) {
            cache.get_or_build("stock-999", true, [&] {
                ++builds[28];
                return make_lut(999.0);
            });
        }
        const auto bounded = cache.stats();
        assert(builds[28] == 2);
        assert(bounded.pinned_entries == 28);
        assert(bounded.entry_limit_bypasses == 2);
        assert(bounded.cache_held_bytes ==
               bounded.pinned_bytes + bounded.dynamic_bytes);
        const auto global = global_budget.snapshot();
        assert(global.domains[static_cast<std::size_t>(
                   spk::memory::MemoryDomain::Cache)].current_bytes ==
               bounded.cache_held_bytes);

        cache.clear();
        assert(cache.stats().cache_held_bytes == 0);
        assert(global_budget.snapshot().domains[static_cast<std::size_t>(
                   spk::memory::MemoryDomain::Cache)].current_bytes == 0);
    }

    // An oversized dynamic value never flushes a useful working set. It is
    // returned uncached and rebuilt on the next request by explicit policy.
    {
        spk::memory::MemoryBudget global_budget(1024u * 1024u);
        spk::TcLutCacheConfig config;
        config.dynamic_byte_budget = 1024;
        config.memory_budget = &global_budget;
        spk::TcLutCache cache(config);
        cache.get_or_build("normal-0", false, [] { return make_lut(1.0); });
        cache.get_or_build("normal-1", false, [] { return make_lut(2.0); });
        const auto before = cache.stats();
        std::uint64_t huge_builds = 0;
        for (int attempt = 0; attempt < 2; ++attempt) {
            cache.get_or_build("huge-lut", false, [&] {
                ++huge_builds;
                return make_lut(4.0, 4096);
            });
        }
        const auto after = cache.stats();
        assert(huge_builds == 2);
        assert(after.oversize_bypasses == before.oversize_bypasses + 2);
        assert(after.evictions == before.evictions);
        assert(after.dynamic_entries == before.dynamic_entries);
        assert(after.dynamic_bytes == before.dynamic_bytes);
    }

    // Concurrent callers for one pure key may race the outside-lock build, but
    // every caller must receive the one resident byte-identical value and cache
    // state/counters must remain coherent.
    {
        constexpr int kThreads = 16;
        spk::memory::MemoryBudget global_budget(1024u * 1024u);
        spk::TcLutCacheConfig config;
        config.dynamic_byte_budget = 64u * 1024u;
        config.memory_budget = &global_budget;
        spk::TcLutCache cache(config);
        std::atomic<std::uint64_t> builds{0};
        std::mutex start_mutex;
        std::condition_variable start_cv;
        int ready = 0;
        bool go = false;
        std::array<spk::TcLutCache::Lease, kThreads> leases;
        std::array<std::thread, kThreads> threads;
        for (int i = 0; i < kThreads; ++i) {
            threads[static_cast<std::size_t>(i)] = std::thread([&, i] {
                {
                    std::unique_lock<std::mutex> lock(start_mutex);
                    ++ready;
                    start_cv.notify_all();
                    start_cv.wait(lock, [&] { return go; });
                }
                leases[static_cast<std::size_t>(i)] =
                    cache.get_or_build("same-slider-key", false, [&] {
                        builds.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::yield();
                        return make_lut(42.0);
                    });
            });
        }
        {
            std::unique_lock<std::mutex> lock(start_mutex);
            start_cv.wait(lock, [&] { return ready == kThreads; });
            go = true;
        }
        start_cv.notify_all();
        for (auto& thread : threads) thread.join();

        for (int i = 0; i < kThreads; ++i) {
            assert(leases[static_cast<std::size_t>(i)]);
            assert(leases[static_cast<std::size_t>(i)].get() == leases[0].get());
            assert(std::memcmp(leases[static_cast<std::size_t>(i)]->data.data(),
                               leases[0]->data.data(),
                               leases[0]->data.size() * sizeof(double)) == 0);
        }
        const auto concurrent = cache.stats();
        assert(concurrent.dynamic_entries == 1);
        assert(concurrent.misses == builds.load(std::memory_order_relaxed));
        assert(concurrent.hits + concurrent.race_hits == kThreads - 1);
        assert(concurrent.cache_held_bytes == concurrent.dynamic_bytes);
    }

    // Build/allocation failure is failure-atomic. Global-budget denial has an
    // explicit uncached fallback and records both cache and coordinator stats.
    {
        spk::memory::MemoryBudget global_budget(1);
        spk::TcLutCacheConfig config;
        config.dynamic_byte_budget = 64u * 1024u;
        config.memory_budget = &global_budget;
        spk::TcLutCache cache(config);

        bool saw_bad_alloc = false;
        try {
            cache.get_or_build("allocation-failure", false, []() -> spk::NdArray {
                throw std::bad_alloc();
            });
        } catch (const std::bad_alloc&) {
            saw_bad_alloc = true;
        }
        assert(saw_bad_alloc);
        assert(cache.stats().build_failures == 1);
        assert(cache.stats().cache_held_bytes == 0);

        std::uint64_t denied_builds = 0;
        auto denied0 = cache.get_or_build("budget-denied", false, [&] {
            ++denied_builds;
            return make_lut(7.0);
        });
        auto denied1 = cache.get_or_build("budget-denied", false, [&] {
            ++denied_builds;
            return make_lut(7.0);
        });
        assert(denied0 && denied1);
        assert(denied_builds == 2);
        assert(cache.stats().global_budget_denials == 2);
        assert(cache.stats().dynamic_entries == 0);
        const auto denied_snapshot = global_budget.snapshot();
        const auto& cache_domain = denied_snapshot.domains[
            static_cast<std::size_t>(spk::memory::MemoryDomain::Cache)];
        assert(cache_domain.current_bytes == 0);
        assert(cache_domain.denial_count == 2);
    }

    // Eviction removes residency immediately, while the global reservation
    // follows an outstanding render lease until that reader releases it.
    {
        spk::memory::MemoryBudget probe_budget(1024u * 1024u);
        spk::TcLutCacheConfig probe_config;
        probe_config.dynamic_byte_budget = 1024u * 1024u;
        probe_config.memory_budget = &probe_budget;
        spk::TcLutCache probe(probe_config);
        probe.get_or_build("lease-key-a", false, [] { return make_lut(1.0); });
        const std::size_t charge = probe.stats().dynamic_bytes;

        spk::memory::MemoryBudget global_budget(1024u * 1024u);
        spk::TcLutCacheConfig config;
        config.dynamic_byte_budget = charge;
        config.memory_budget = &global_budget;
        spk::TcLutCache cache(config);
        auto held = cache.get_or_build("lease-key-a", false,
                                       [] { return make_lut(1.0); });
        cache.get_or_build("lease-key-b", false, [] { return make_lut(2.0); });
        const auto resident = cache.stats();
        const std::uint64_t while_held = global_budget.snapshot().domains[
            static_cast<std::size_t>(spk::memory::MemoryDomain::Cache)].current_bytes;
        assert(resident.dynamic_entries == 1);
        assert(resident.evictions == 1);
        assert(while_held == resident.cache_held_bytes + charge);
        cache.clear();
        assert(cache.stats().cache_held_bytes == 0);
        assert(held->data[0] == 1.0);
        assert(global_budget.snapshot().domains[static_cast<std::size_t>(
                   spk::memory::MemoryDomain::Cache)].current_bytes == charge);
        held.reset();
        assert(global_budget.snapshot().domains[static_cast<std::size_t>(
                   spk::memory::MemoryDomain::Cache)].current_bytes == 0);
        cache.clear();
        assert(global_budget.snapshot().total.current_bytes == 0);
    }

    // The final reservation cannot become reusable while any charged payload
    // is still live. Pause the releasing thread after key/NdArray/node/control
    // allocations have all been deallocated but before MemoryBudget::release;
    // a concurrent same-sized admission must deterministically fail until the
    // barrier opens, then succeed.
    {
        const std::size_t charge = [] {
            spk::memory::MemoryBudget probe_budget(1024u * 1024u);
            spk::TcLutCacheConfig probe_config;
            probe_config.dynamic_byte_budget = 64u * 1024u;
            probe_config.memory_budget = &probe_budget;
            spk::TcLutCache probe(probe_config);
            auto lease = probe.get_or_build(
                "release-admit", false, [] { return make_lut(1.0); });
            const std::size_t measured = probe.stats().dynamic_bytes;
            probe.clear();
            lease.reset();
            assert(probe_budget.snapshot().total.current_bytes == 0);
            return measured;
        }();
        assert(charge > 0);

        spk::memory::MemoryBudget global_budget(charge);
        PayloadTeardownBarrier barrier;
        spk::TcLutCacheConfig releasing_config;
        releasing_config.dynamic_byte_budget = 64u * 1024u;
        releasing_config.memory_budget = &global_budget;
        releasing_config.post_payload_teardown_hook_for_testing =
            &PayloadTeardownBarrier::wait_before_budget_release;
        releasing_config.post_payload_teardown_context_for_testing = &barrier;
        spk::TcLutCache releasing_cache(releasing_config);

        auto held = releasing_cache.get_or_build(
            "release-admit", false, [] { return make_lut(1.0); });
        assert(releasing_cache.stats().dynamic_bytes == charge);
        releasing_cache.clear();

        std::thread releaser([lease = std::move(held)]() mutable {
            lease.reset();
        });
        {
            std::unique_lock<std::mutex> lock(barrier.mutex);
            barrier.cv.wait(lock, [&] { return barrier.payload_deallocated; });
        }
        assert(global_budget.snapshot().total.current_bytes == charge);

        spk::TcLutCacheConfig admitting_config;
        admitting_config.dynamic_byte_budget = 64u * 1024u;
        admitting_config.memory_budget = &global_budget;
        spk::TcLutCache admitting_cache(admitting_config);
        std::uint64_t admitting_builds = 0;
        auto denied = admitting_cache.get_or_build(
            "release-admit", false, [&] {
                ++admitting_builds;
                return make_lut(2.0);
            });
        assert(denied);
        assert(admitting_cache.stats().global_budget_denials == 1);
        assert(admitting_cache.stats().dynamic_entries == 0);
        assert(global_budget.snapshot().total.current_bytes == charge);

        {
            std::lock_guard<std::mutex> lock(barrier.mutex);
            barrier.allow_budget_release = true;
        }
        barrier.cv.notify_all();
        releaser.join();
        assert(global_budget.snapshot().total.current_bytes == 0);

        denied.reset();
        auto admitted = admitting_cache.get_or_build(
            "release-admit", false, [&] {
                ++admitting_builds;
                return make_lut(2.0);
            });
        assert(admitted);
        assert(admitting_builds == 2);
        assert(admitting_cache.stats().dynamic_entries == 1);
        assert(admitting_cache.stats().dynamic_bytes == charge);
        assert(global_budget.snapshot().total.current_bytes == charge);
        admitting_cache.clear();
        assert(global_budget.snapshot().total.current_bytes == charge);
        admitted.reset();
        assert(global_budget.snapshot().total.current_bytes == 0);
    }

    return 0;
}

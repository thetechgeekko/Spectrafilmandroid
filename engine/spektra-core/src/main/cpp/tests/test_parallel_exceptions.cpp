/*
 * Spektrafilm for Android — host regression for fork-join exception safety.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
#include "kernels/parallel.h"

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

void set_test_environment(const char* name, const char* value) {
#if defined(_WIN32)
    const int result = _putenv_s(name, value);
#else
    const int result = setenv(name, value, 1);
#endif
    // Keep the mutation outside assert(): Release builds define NDEBUG, and dropping this call
    // silently serializes the test and makes the owner-waits-for-worker scenario deadlock.
    assert(result == 0);
}

int never_cancel(void*) noexcept {
    return 0;
}

}  // namespace

int main() {
    set_test_environment("SPK_NUM_THREADS", "4");
    set_test_environment("SPK_PARALLEL_MIN_CHUNK", "1");

    // An allocation failure originating on a worker must reach the owner thread
    // as std::bad_alloc, not escape std::thread and terminate the process.
    bool caught_bad_alloc = false;
    try {
        spk::parallel_for(0, 4, [](int begin, int) {
            if (begin > 0) throw std::bad_alloc{};
        });
    } catch (const std::bad_alloc&) {
        caught_bad_alloc = true;
    }
    assert(caught_bad_alloc);

    // An owner-thread exception must also join already-running workers before
    // it is rethrown. Destroying a vector of joinable threads would terminate.
    std::atomic<int> active_workers{0};
    std::atomic<bool> worker_started{false};
    std::atomic<bool> owner_threw{false};
    bool caught_owner_failure = false;
    try {
        spk::parallel_for(0, 4, [&](int begin, int) {
            if (begin == 0) {
                while (!worker_started.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                owner_threw.store(true, std::memory_order_release);
                throw std::runtime_error("owner failure");
            }
            active_workers.fetch_add(1, std::memory_order_release);
            worker_started.store(true, std::memory_order_release);
            while (!owner_threw.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            active_workers.fetch_sub(1, std::memory_order_release);
        });
    } catch (const std::runtime_error& failure) {
        caught_owner_failure =
            std::string(failure.what()) == "owner failure";
    }
    assert(caught_owner_failure);
    assert(active_workers.load(std::memory_order_acquire) == 0);

    // The cancellation-aware dispatcher uses the same exception contract. A
    // real worker failure wins over a non-cancelling callback and is rethrown
    // only after the worker pool has drained.
    bool caught_cancellable_failure = false;
    try {
        spk::ParallelCancellationScope cancellation(never_cancel, nullptr);
        spk::parallel_for(0, 4, [](int begin, int) {
            if (begin > 0) throw std::logic_error("cancellable worker failure");
        });
    } catch (const std::logic_error& failure) {
        caught_cancellable_failure =
            std::string(failure.what()) == "cancellable worker failure";
    }
    assert(caught_cancellable_failure);

    // Grain uses the dynamic dispatcher because independently-seeded blocks
    // have uneven cost. Its workers have the same no-exception-escape contract.
    bool caught_dynamic_failure = false;
    try {
        std::atomic<int> calls{0};
        spk::detail::parallel_dispatch_dynamic(
            4, [&](const std::atomic<bool>*) {
                if (calls.fetch_add(1, std::memory_order_relaxed) == 0) {
                    throw std::bad_alloc{};
                }
            });
    } catch (const std::bad_alloc&) {
        caught_dynamic_failure = true;
    }
    assert(caught_dynamic_failure);

    // Deterministically inject failure after one dynamic worker has launched.
    // The helper must stop and join that live worker before surfacing the
    // construction error; vector unwinding with a joinable thread would abort.
    bool caught_construction_failure = false;
    int launches = 0;
    try {
        const auto failing_factory = [&](auto&& entry) -> std::thread {
            if (launches++ == 1) {
                throw std::system_error(
                    std::make_error_code(std::errc::resource_unavailable_try_again));
            }
            return std::thread(std::forward<decltype(entry)>(entry));
        };
        spk::detail::parallel_dispatch_dynamic_with_factory(
            3,
            [](const std::atomic<bool>* stop) {
                while (!stop->load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            },
            failing_factory);
    } catch (const std::system_error& failure) {
        caught_construction_failure =
            failure.code() ==
            std::make_error_code(std::errc::resource_unavailable_try_again);
    }
    assert(caught_construction_failure);
    assert(launches == 2);

    // A contained failure must not poison later dispatches in the same process.
    std::vector<int> output(16, 0);
    spk::parallel_for(0, static_cast<int>(output.size()),
                      [&](int begin, int end) {
        for (int i = begin; i < end; ++i) output[static_cast<std::size_t>(i)] = i + 1;
    });
    for (std::size_t i = 0; i < output.size(); ++i) {
        assert(output[i] == static_cast<int>(i + 1));
    }
}

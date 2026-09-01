/*
 * Spektrafilm for Android — bounded memo for filming transfer-characteristic LUTs.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
#ifndef SPK_RUNTIME_TC_LUT_CACHE_H
#define SPK_RUNTIME_TC_LUT_CACHE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "io/npy_lut.h"
#include "runtime/memory_budget.h"

namespace spk {

struct TcLutCacheConfig {
    using PostPayloadTeardownHookForTesting = void (*)(void*) noexcept;

    static constexpr std::size_t kDefaultDynamicByteBudget =
        8u * 1024u * 1024u;
    static constexpr std::size_t kDefaultPinnedByteBudget =
        28u * 1024u * 1024u;
    static constexpr std::size_t kDefaultMaxPinnedEntries = 28;
    static constexpr std::size_t kDefaultMaxDynamicEntries = 64;

    std::size_t dynamic_byte_budget = kDefaultDynamicByteBudget;
    std::size_t pinned_byte_budget = kDefaultPinnedByteBudget;
    std::size_t max_pinned_entries = kDefaultMaxPinnedEntries;
    std::size_t max_dynamic_entries = kDefaultMaxDynamicEntries;
    memory::MemoryBudget* memory_budget = nullptr;

    // Deterministic host-test seam. Production leaves this null. An admitted
    // node invokes it after key/value/node/control-block storage is deallocated
    // but immediately before its global MemoryReservation is released.
    PostPayloadTeardownHookForTesting post_payload_teardown_hook_for_testing =
        nullptr;
    void* post_payload_teardown_context_for_testing = nullptr;
};

struct TcLutCacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t race_hits = 0;
    std::uint64_t evictions = 0;
    std::uint64_t oversize_bypasses = 0;
    std::uint64_t entry_limit_bypasses = 0;
    std::uint64_t budget_bypasses = 0;
    std::uint64_t global_budget_denials = 0;
    std::uint64_t build_failures = 0;
    std::uint64_t accounting_overflows = 0;
    std::uint64_t classification_conflicts = 0;
    // Bytes for nodes still held by this cache. Eviction/clear subtracts these
    // immediately. The process MemoryDomain::Cache counter can remain higher
    // while an already-running render still owns an evicted alias lease.
    std::size_t cache_held_bytes = 0;
    std::size_t pinned_bytes = 0;
    std::size_t dynamic_bytes = 0;
    std::size_t pinned_entries = 0;
    std::size_t dynamic_entries = 0;
    std::size_t pinned_byte_budget = 0;
    std::size_t dynamic_byte_budget = 0;
    std::size_t max_pinned_entries = 0;
    std::size_t max_dynamic_entries = 0;
};

// A cache hit and a cache miss both return an immutable shared lease. Keeping
// ownership in the lease makes LRU eviction safe while another render is still
// reading that LUT.
//
// MEMORY-BUDGET BOUNDARY: exact owner/key/vector allocation requests are charged
// to MemoryDomain::Cache when (and only when) the completed node is admitted.
// The builder must run first because its NdArray capacities determine that exact
// charge. Therefore build scratch and an uncached bypass/denial result are
// transient allocations outside this cache-residency admission; this class must
// not be cited as admission-before-allocation or full native-domain coverage.
// Those broader render/scratch domains remain tracked by #176.
class TcLutCache final {
public:
    using Lease = std::shared_ptr<const NdArray>;
    using Builder = std::function<NdArray()>;

    static constexpr std::size_t kHardMaxPinnedEntries = 28;
    static constexpr std::size_t kHardMaxDynamicEntries = 64;
    static constexpr std::size_t kHardMaxEntries =
        kHardMaxPinnedEntries + kHardMaxDynamicEntries;

    explicit TcLutCache(TcLutCacheConfig config = {});
    ~TcLutCache();

    TcLutCache(const TcLutCache&) = delete;
    TcLutCache& operator=(const TcLutCache&) = delete;

    Lease get_or_build(const std::string& key, bool pin,
                       const Builder& builder);
    TcLutCacheStats stats() const;
    void clear() noexcept;
    void set_dynamic_byte_budget(std::size_t bytes) noexcept;

private:
    struct Node;

    static bool key_equals(const Node& node, const std::string& key) noexcept;
    static Lease lease(const std::shared_ptr<Node>& node) noexcept;
    std::uint64_t next_recency_locked() noexcept;
    bool evict_oldest_dynamic_locked() noexcept;

    mutable std::mutex mutex_;
    TcLutCacheConfig config_;
    std::array<std::shared_ptr<Node>, kHardMaxEntries> entries_{};
    TcLutCacheStats stats_{};
    std::uint64_t recency_clock_ = 0;
};

}  // namespace spk

#endif  // SPK_RUNTIME_TC_LUT_CACHE_H

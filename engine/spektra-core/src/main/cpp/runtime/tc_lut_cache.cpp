/*
 * Spektrafilm for Android — bounded memo for filming transfer-characteristic LUTs.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
#include "runtime/tc_lut_cache.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace spk {
namespace {

struct AllocationLifetime {
    using Hook = TcLutCacheConfig::PostPayloadTeardownHookForTesting;

    // This separately allocated owner is cache bookkeeping too. It is included
    // in the exact charge, deleted before the reservation is released, and
    // leaves only a stack-local MemoryReservation during the final reset.
    std::size_t bytes = sizeof(AllocationLifetime);
    bool overflow = false;
    std::size_t outstanding_allocations = 0;
    bool armed = false;
    memory::MemoryReservation reservation;
    Hook post_payload_teardown_hook = nullptr;
    void* post_payload_teardown_context = nullptr;

    void allocation_succeeded(std::size_t amount) noexcept {
        if (outstanding_allocations ==
            std::numeric_limits<std::size_t>::max()) {
            std::terminate();
        }
        ++outstanding_allocations;
        if (amount > std::numeric_limits<std::size_t>::max() - bytes) {
            overflow = true;
            bytes = std::numeric_limits<std::size_t>::max();
            return;
        }
        bytes += amount;
    }

    void allocation_deallocated() noexcept {
        if (outstanding_allocations == 0) std::terminate();
        --outstanding_allocations;
        if (armed && outstanding_allocations == 0) finish();
    }

    void arm(memory::MemoryReservation&& owned_reservation, Hook hook,
             void* hook_context) noexcept {
        if (armed) std::terminate();
        reservation = std::move(owned_reservation);
        if (reservation) {
            post_payload_teardown_hook = hook;
            post_payload_teardown_context = hook_context;
        }
        armed = true;
        if (outstanding_allocations == 0) finish();
    }

private:
    void finish() noexcept {
        // The allocator calls this only after its final underlying deallocation.
        // Move the reservation to stack storage, free this last charged metadata,
        // let the deterministic test barrier observe that state, then uncharge.
        memory::MemoryReservation release_after_deallocation =
            std::move(reservation);
        const Hook hook = post_payload_teardown_hook;
        void* const hook_context = post_payload_teardown_context;
        delete this;
        if (release_after_deallocation && hook) hook(hook_context);
        release_after_deallocation.reset();
    }
};

class AllocationLifetimeArmer final {
public:
    explicit AllocationLifetimeArmer(AllocationLifetime* lifetime) noexcept
        : lifetime_(lifetime) {}

    ~AllocationLifetimeArmer() {
        if (!lifetime_) return;
        memory::MemoryReservation none;
        lifetime_->arm(std::move(none), nullptr, nullptr);
    }

    AllocationLifetimeArmer(const AllocationLifetimeArmer&) = delete;
    AllocationLifetimeArmer& operator=(const AllocationLifetimeArmer&) = delete;

    void arm(memory::MemoryReservation&& reservation,
             AllocationLifetime::Hook hook, void* hook_context) noexcept {
        lifetime_->arm(std::move(reservation), hook, hook_context);
        lifetime_ = nullptr;
    }

private:
    AllocationLifetime* lifetime_;
};

template <typename T>
class TallyAllocator {
public:
    using value_type = T;

    TallyAllocator() noexcept = default;
    explicit TallyAllocator(AllocationLifetime* lifetime) noexcept
        : lifetime_(lifetime) {}

    template <typename U>
    TallyAllocator(const TallyAllocator<U>& other) noexcept
        : lifetime_(other.lifetime()) {}

    T* allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        T* const result = std::allocator<T>{}.allocate(count);
        if (lifetime_) {
            lifetime_->allocation_succeeded(count * sizeof(T));
        }
        return result;
    }

    void deallocate(T* ptr, std::size_t count) noexcept {
        AllocationLifetime* const lifetime = lifetime_;
        std::allocator<T>{}.deallocate(ptr, count);
        // Do not touch this allocator after freeing the underlying block: an
        // implementation may store it inside allocate_shared's control block.
        if (lifetime) lifetime->allocation_deallocated();
    }

    AllocationLifetime* lifetime() const noexcept { return lifetime_; }

    template <typename U>
    bool operator==(const TallyAllocator<U>& other) const noexcept {
        return lifetime_ == other.lifetime();
    }
    template <typename U>
    bool operator!=(const TallyAllocator<U>& other) const noexcept {
        return !(*this == other);
    }

private:
    AllocationLifetime* lifetime_ = nullptr;
};

using TrackedString =
    std::basic_string<char, std::char_traits<char>, TallyAllocator<char>>;

bool checked_vector_bytes(std::size_t count, std::size_t element_bytes,
                          std::size_t* result) noexcept {
    if (!result || (element_bytes != 0 &&
                    count > std::numeric_limits<std::size_t>::max() /
                                element_bytes)) {
        return false;
    }
    *result = count * element_bytes;
    return true;
}

bool checked_add(std::size_t value, std::size_t increment,
                 std::size_t* result) noexcept {
    if (!result || increment > std::numeric_limits<std::size_t>::max() - value) {
        return false;
    }
    *result = value + increment;
    return true;
}

void saturating_increment(std::uint64_t* value) noexcept {
    if (value && *value != std::numeric_limits<std::uint64_t>::max()) ++*value;
}

}  // namespace

struct TcLutCache::Node {
    Node(const std::string& cache_key, NdArray&& built,
         AllocationLifetime* lifetime)
        : key(cache_key.begin(), cache_key.end(),
              TallyAllocator<char>(lifetime)),
          value(std::move(built)) {}

    TrackedString key;
    NdArray value;
    std::size_t charged_bytes = 0;
    bool pinned = false;
    std::uint64_t recency = 0;
};

TcLutCache::TcLutCache(TcLutCacheConfig config) : config_(config) {
    config_.max_pinned_entries =
        std::min(config_.max_pinned_entries, kHardMaxPinnedEntries);
    config_.max_dynamic_entries =
        std::min(config_.max_dynamic_entries, kHardMaxDynamicEntries);
    if (!config_.memory_budget) {
        config_.memory_budget = &memory::process_memory_budget();
    }
}

TcLutCache::~TcLutCache() = default;

bool TcLutCache::key_equals(const Node& node,
                            const std::string& key) noexcept {
    return node.key.size() == key.size() &&
           std::char_traits<char>::compare(node.key.data(), key.data(),
                                           key.size()) == 0;
}

TcLutCache::Lease TcLutCache::lease(
        const std::shared_ptr<Node>& node) noexcept {
    return Lease(node, &node->value);
}

std::uint64_t TcLutCache::next_recency_locked() noexcept {
    if (recency_clock_ != std::numeric_limits<std::uint64_t>::max()) {
        return ++recency_clock_;
    }

    // The counter is diagnostic-internal but a wrap must not invert LRU order.
    // With at most 64 dynamic nodes, ranking the live stamps is deterministic
    // and bounded; pinned nodes do not participate in eviction.
    std::uint64_t rank = 0;
    for (;;) {
        Node* next = nullptr;
        for (const auto& entry : entries_) {
            if (!entry || entry->pinned || entry->recency <= rank) continue;
            if (!next || entry->recency < next->recency) next = entry.get();
        }
        if (!next) break;
        next->recency = ++rank;
    }
    recency_clock_ = rank;
    return ++recency_clock_;
}

bool TcLutCache::evict_oldest_dynamic_locked() noexcept {
    auto oldest = entries_.end();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (!*it || (*it)->pinned) continue;
        if (oldest == entries_.end() ||
            (*it)->recency < (*oldest)->recency) {
            oldest = it;
        }
    }
    if (oldest == entries_.end()) return false;

    const std::size_t charged = (*oldest)->charged_bytes;
    stats_.cache_held_bytes -= charged;
    stats_.dynamic_bytes -= charged;
    --stats_.dynamic_entries;
    saturating_increment(&stats_.evictions);
    oldest->reset();
    return true;
}

TcLutCache::Lease TcLutCache::get_or_build(const std::string& key, bool pin,
                                           const Builder& builder) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        for (const auto& entry : entries_) {
            if (entry && key_equals(*entry, key)) {
                if (entry->pinned != pin) {
                    saturating_increment(&stats_.classification_conflicts);
                }
                saturating_increment(&stats_.hits);
                if (!entry->pinned) entry->recency = next_recency_locked();
                return lease(entry);
            }
        }
        saturating_increment(&stats_.misses);
    }

    std::unique_ptr<AllocationLifetime> lifetime;
    std::shared_ptr<Node> node;
    try {
        NdArray built = builder();
        lifetime = std::make_unique<AllocationLifetime>();
        node = std::allocate_shared<Node>(TallyAllocator<Node>(lifetime.get()),
                                          key, std::move(built), lifetime.get());
    } catch (...) {
        std::lock_guard<std::mutex> guard(mutex_);
        saturating_increment(&stats_.build_failures);
        throw;
    }
    AllocationLifetime* const allocation_lifetime = lifetime.release();
    AllocationLifetimeArmer lifetime_armer(allocation_lifetime);

    std::size_t shape_bytes = 0;
    std::size_t data_bytes = 0;
    std::size_t charged = allocation_lifetime->bytes;
    const bool accounted = !allocation_lifetime->overflow &&
        checked_vector_bytes(node->value.shape.capacity(), sizeof(int),
                             &shape_bytes) &&
        checked_vector_bytes(node->value.data.capacity(), sizeof(double),
                             &data_bytes) &&
        checked_add(charged, shape_bytes, &charged) &&
        checked_add(charged, data_bytes, &charged);
    if (!accounted) {
        std::lock_guard<std::mutex> guard(mutex_);
        saturating_increment(&stats_.accounting_overflows);
        return lease(node);
    }

    std::lock_guard<std::mutex> guard(mutex_);
    for (const auto& entry : entries_) {
        if (entry && key_equals(*entry, key)) {
            if (entry->pinned != pin) {
                saturating_increment(&stats_.classification_conflicts);
            }
            saturating_increment(&stats_.race_hits);
            if (!entry->pinned) entry->recency = next_recency_locked();
            return lease(entry);
        }
    }

    const std::size_t byte_limit =
        pin ? config_.pinned_byte_budget : config_.dynamic_byte_budget;
    const std::size_t entry_limit =
        pin ? config_.max_pinned_entries : config_.max_dynamic_entries;
    const std::size_t current_bytes =
        pin ? stats_.pinned_bytes : stats_.dynamic_bytes;
    const std::size_t current_entries =
        pin ? stats_.pinned_entries : stats_.dynamic_entries;
    if (charged > byte_limit) {
        saturating_increment(&stats_.oversize_bypasses);
        return lease(node);
    }

    if (pin && (current_bytes > byte_limit - charged ||
                current_entries >= entry_limit)) {
        if (current_entries >= entry_limit) {
            saturating_increment(&stats_.entry_limit_bypasses);
        } else {
            saturating_increment(&stats_.budget_bypasses);
        }
        return lease(node);
    }
    if (!pin) {
        while ((stats_.dynamic_bytes > byte_limit - charged ||
                stats_.dynamic_entries >= entry_limit) &&
               evict_oldest_dynamic_locked()) {}
        if (stats_.dynamic_bytes > byte_limit - charged ||
            stats_.dynamic_entries >= entry_limit) {
            if (stats_.dynamic_entries >= entry_limit) {
                saturating_increment(&stats_.entry_limit_bypasses);
            } else {
                saturating_increment(&stats_.budget_bypasses);
            }
            return lease(node);
        }
    }

    std::size_t new_cache_held_bytes = 0;
    if (!checked_add(stats_.cache_held_bytes, charged,
                     &new_cache_held_bytes)) {
        saturating_increment(&stats_.accounting_overflows);
        return lease(node);
    }

    auto empty = std::find(entries_.begin(), entries_.end(), nullptr);
    if (empty == entries_.end()) {
        saturating_increment(&stats_.entry_limit_bypasses);
        return lease(node);
    }

    static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t),
                  "MemoryBudget cannot represent size_t cache charges");
    auto reservation = config_.memory_budget->try_reserve(
        static_cast<std::uint64_t>(charged), memory::MemoryDomain::Cache,
        memory::MemoryStage::Lut);
    if (!reservation) {
        saturating_increment(&stats_.global_budget_denials);
        return lease(node);
    }

    node->charged_bytes = charged;
    node->pinned = pin;
    node->recency = pin ? 0 : next_recency_locked();
    lifetime_armer.arm(
        std::move(reservation),
        config_.post_payload_teardown_hook_for_testing,
        config_.post_payload_teardown_context_for_testing);
    *empty = node;
    stats_.cache_held_bytes = new_cache_held_bytes;
    if (pin) {
        stats_.pinned_bytes += charged;
        ++stats_.pinned_entries;
    } else {
        stats_.dynamic_bytes += charged;
        ++stats_.dynamic_entries;
    }
    return lease(node);
}

TcLutCacheStats TcLutCache::stats() const {
    std::lock_guard<std::mutex> guard(mutex_);
    TcLutCacheStats snapshot = stats_;
    snapshot.pinned_byte_budget = config_.pinned_byte_budget;
    snapshot.dynamic_byte_budget = config_.dynamic_byte_budget;
    snapshot.max_pinned_entries = config_.max_pinned_entries;
    snapshot.max_dynamic_entries = config_.max_dynamic_entries;
    return snapshot;
}

void TcLutCache::clear() noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto& entry : entries_) entry.reset();
    stats_.cache_held_bytes = 0;
    stats_.pinned_bytes = 0;
    stats_.dynamic_bytes = 0;
    stats_.pinned_entries = 0;
    stats_.dynamic_entries = 0;
}

void TcLutCache::set_dynamic_byte_budget(std::size_t bytes) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    config_.dynamic_byte_budget = bytes;
    while (stats_.dynamic_bytes > bytes && evict_oldest_dynamic_locked()) {}
}

}  // namespace spk

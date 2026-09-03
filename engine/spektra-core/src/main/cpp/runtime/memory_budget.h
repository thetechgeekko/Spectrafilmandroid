/*
 * Spektrafilm for Android — process-wide native memory admission and diagnostics.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
#ifndef SPK_RUNTIME_MEMORY_BUDGET_H
#define SPK_RUNTIME_MEMORY_BUDGET_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace spk::memory {

// Numeric values are diagnostic ABI: append new entries before Count and never
// renumber existing entries. Domains describe where bytes live; stages describe
// the operation that requested them.
enum class MemoryDomain : std::uint8_t {
    Unknown = 0,
    JniOwned = 1,
    NativeImage = 2,
    NativeScratch = 3,
    Cache = 4,
    GpuHost = 5,
    GpuDevice = 6,
    Writer = 7,
    Jvm = 8,
    Count = 9,
};

enum class MemoryStage : std::uint8_t {
    Unknown = 0,
    JniSimResult = 1,
    JniDirectBuffer = 2,
    Decode = 3,
    Filming = 4,
    Scanning = 5,
    Printing = 6,
    Spatial = 7,
    Grain = 8,
    Lut = 9,
    Writer = 10,
    Gpu = 11,
    Count = 12,
};

constexpr std::size_t kMemoryDomainCount =
    static_cast<std::size_t>(MemoryDomain::Count);
constexpr std::size_t kMemoryStageCount =
    static_cast<std::size_t>(MemoryStage::Count);

inline const char* memory_domain_name(MemoryDomain domain) noexcept {
    switch (domain) {
        case MemoryDomain::Unknown: return "unknown";
        case MemoryDomain::JniOwned: return "jni_owned";
        case MemoryDomain::NativeImage: return "native_image";
        case MemoryDomain::NativeScratch: return "native_scratch";
        case MemoryDomain::Cache: return "cache";
        case MemoryDomain::GpuHost: return "gpu_host";
        case MemoryDomain::GpuDevice: return "gpu_device";
        case MemoryDomain::Writer: return "writer";
        case MemoryDomain::Jvm: return "jvm";
        case MemoryDomain::Count: break;
    }
    return "unknown";
}

inline const char* memory_stage_name(MemoryStage stage) noexcept {
    switch (stage) {
        case MemoryStage::Unknown: return "unknown";
        case MemoryStage::JniSimResult: return "jni_sim_result";
        case MemoryStage::JniDirectBuffer: return "jni_direct_buffer";
        case MemoryStage::Decode: return "decode";
        case MemoryStage::Filming: return "filming";
        case MemoryStage::Scanning: return "scanning";
        case MemoryStage::Printing: return "printing";
        case MemoryStage::Spatial: return "spatial";
        case MemoryStage::Grain: return "grain";
        case MemoryStage::Lut: return "lut";
        case MemoryStage::Writer: return "writer";
        case MemoryStage::Gpu: return "gpu";
        case MemoryStage::Count: break;
    }
    return "unknown";
}

struct MemoryCounterSnapshot {
    std::uint64_t current_bytes = 0;
    std::uint64_t peak_bytes = 0;
    std::uint64_t denial_count = 0;
    std::uint64_t denied_bytes = 0;
};

struct MemoryBudgetSnapshot {
    std::uint64_t limit_bytes = 0;
    MemoryCounterSnapshot total{};
    std::array<MemoryCounterSnapshot, kMemoryDomainCount> domains{};
    std::array<MemoryCounterSnapshot, kMemoryStageCount> stages{};
};

inline void append_memory_counter_json(
        std::string& output, const MemoryCounterSnapshot& counter) {
    output += "{\"current_bytes\":";
    output += std::to_string(counter.current_bytes);
    output += ",\"peak_bytes\":";
    output += std::to_string(counter.peak_bytes);
    output += ",\"denial_count\":";
    output += std::to_string(counter.denial_count);
    output += ",\"denied_bytes\":";
    output += std::to_string(counter.denied_bytes);
    output += '}';
}

// Stable, deterministic ordering makes release traces diffable and avoids a
// JSON-library dependency in the JNI bridge.
inline std::string memory_budget_snapshot_json(
        const MemoryBudgetSnapshot& snapshot) {
    std::string output;
    output.reserve(4096);
    output += "{\"schema\":\"spk.memory_budget.v1\",\"limit_bytes\":";
    output += std::to_string(snapshot.limit_bytes);
    output += ",\"total\":";
    append_memory_counter_json(output, snapshot.total);
    output += ",\"domains\":[";
    for (std::size_t i = 0; i < kMemoryDomainCount; ++i) {
        if (i != 0) output += ',';
        output += "{\"id\":";
        output += std::to_string(i);
        output += ",\"name\":\"";
        output += memory_domain_name(static_cast<MemoryDomain>(i));
        output += "\",";
        std::string counter;
        append_memory_counter_json(counter, snapshot.domains[i]);
        output += counter.substr(1);
    }
    output += "],\"stages\":[";
    for (std::size_t i = 0; i < kMemoryStageCount; ++i) {
        if (i != 0) output += ',';
        output += "{\"id\":";
        output += std::to_string(i);
        output += ",\"name\":\"";
        output += memory_stage_name(static_cast<MemoryStage>(i));
        output += "\",";
        std::string counter;
        append_memory_counter_json(counter, snapshot.stages[i]);
        output += counter.substr(1);
    }
    output += "]}";
    return output;
}

class MemoryBudget;

// Move-only ownership of one successful admission. Destruction or reset returns
// the exact byte count once, including on exceptions and early returns.
class MemoryReservation final {
public:
    MemoryReservation() noexcept = default;
    ~MemoryReservation() { reset(); }

    MemoryReservation(const MemoryReservation&) = delete;
    MemoryReservation& operator=(const MemoryReservation&) = delete;

    MemoryReservation(MemoryReservation&& other) noexcept { move_from(other); }

    MemoryReservation& operator=(MemoryReservation&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(other);
        }
        return *this;
    }

    explicit operator bool() const noexcept {
        return budget_ != nullptr && bytes_ != 0;
    }

    std::uint64_t bytes() const noexcept { return bytes_; }
    MemoryDomain domain() const noexcept { return domain_; }
    MemoryStage stage() const noexcept { return stage_; }

    void reset() noexcept;

private:
    MemoryReservation(MemoryBudget* budget, std::uint64_t bytes,
                      MemoryDomain domain, MemoryStage stage) noexcept
        : budget_(budget), bytes_(bytes), domain_(domain), stage_(stage) {}

    void move_from(MemoryReservation& other) noexcept {
        budget_ = other.budget_;
        bytes_ = other.bytes_;
        domain_ = other.domain_;
        stage_ = other.stage_;
        other.budget_ = nullptr;
        other.bytes_ = 0;
        other.domain_ = MemoryDomain::Unknown;
        other.stage_ = MemoryStage::Unknown;
    }

    MemoryBudget* budget_ = nullptr;
    std::uint64_t bytes_ = 0;
    MemoryDomain domain_ = MemoryDomain::Unknown;
    MemoryStage stage_ = MemoryStage::Unknown;

    friend class MemoryBudget;
};

class MemoryBudget final {
public:
    explicit MemoryBudget(
            std::uint64_t limit_bytes =
                std::numeric_limits<std::uint64_t>::max()) noexcept
        : limit_bytes_(limit_bytes) {}

    MemoryBudget(const MemoryBudget&) = delete;
    MemoryBudget& operator=(const MemoryBudget&) = delete;

    // Admission is one CAS against the process total. A failed admission never
    // changes current bytes, but records the rejected request for diagnostics.
    MemoryReservation try_reserve(std::uint64_t bytes, MemoryDomain domain,
                                  MemoryStage stage) noexcept {
        if (bytes == 0) return {};
        domain = sanitized_domain(domain);
        stage = sanitized_stage(stage);

        std::uint64_t current = total_.current_bytes.load(std::memory_order_relaxed);
        for (;;) {
            const std::uint64_t limit = limit_bytes_.load(std::memory_order_acquire);
            if (bytes > limit || current > limit - bytes) {
                note_denial(domain, stage, bytes);
                return {};
            }
            const std::uint64_t admitted = current + bytes;
            if (total_.current_bytes.compare_exchange_weak(
                    current, admitted, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                update_peak(total_.peak_bytes, admitted);
                admit(counter_for(domain), bytes);
                admit(counter_for(stage), bytes);
                return MemoryReservation(this, bytes, domain, stage);
            }
        }
    }

    // Lowering the ceiling never invalidates live reservations; subsequent
    // requests are denied until current usage again fits below the new limit.
    void set_limit_bytes(std::uint64_t limit_bytes) noexcept {
        limit_bytes_.store(limit_bytes, std::memory_order_release);
    }

    std::uint64_t limit_bytes() const noexcept {
        return limit_bytes_.load(std::memory_order_acquire);
    }

    MemoryBudgetSnapshot snapshot() const noexcept {
        MemoryBudgetSnapshot result;
        result.limit_bytes = limit_bytes();
        result.total = snapshot_of(total_);
        for (std::size_t i = 0; i < kMemoryDomainCount; ++i) {
            result.domains[i] = snapshot_of(domains_[i]);
        }
        for (std::size_t i = 0; i < kMemoryStageCount; ++i) {
            result.stages[i] = snapshot_of(stages_[i]);
        }
        return result;
    }

private:
    struct AtomicCounter {
        std::atomic<std::uint64_t> current_bytes{0};
        std::atomic<std::uint64_t> peak_bytes{0};
        std::atomic<std::uint64_t> denial_count{0};
        std::atomic<std::uint64_t> denied_bytes{0};
    };

    static MemoryDomain sanitized_domain(MemoryDomain domain) noexcept {
        return static_cast<std::size_t>(domain) < kMemoryDomainCount
                   ? domain
                   : MemoryDomain::Unknown;
    }

    static MemoryStage sanitized_stage(MemoryStage stage) noexcept {
        return static_cast<std::size_t>(stage) < kMemoryStageCount
                   ? stage
                   : MemoryStage::Unknown;
    }

    AtomicCounter& counter_for(MemoryDomain domain) noexcept {
        return domains_[static_cast<std::size_t>(domain)];
    }

    AtomicCounter& counter_for(MemoryStage stage) noexcept {
        return stages_[static_cast<std::size_t>(stage)];
    }

    static void update_peak(std::atomic<std::uint64_t>& peak,
                            std::uint64_t candidate) noexcept {
        std::uint64_t observed = peak.load(std::memory_order_relaxed);
        while (observed < candidate &&
               !peak.compare_exchange_weak(observed, candidate,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed)) {}
    }

    static void saturating_add(std::atomic<std::uint64_t>& value,
                               std::uint64_t increment) noexcept {
        std::uint64_t observed = value.load(std::memory_order_relaxed);
        for (;;) {
            const std::uint64_t maximum =
                std::numeric_limits<std::uint64_t>::max();
            const std::uint64_t desired =
                increment > maximum - observed ? maximum : observed + increment;
            if (value.compare_exchange_weak(observed, desired,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
                return;
            }
        }
    }

    static void admit(AtomicCounter& counter, std::uint64_t bytes) noexcept {
        const std::uint64_t current =
            counter.current_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        update_peak(counter.peak_bytes, current);
    }

    static MemoryCounterSnapshot snapshot_of(
            const AtomicCounter& counter) noexcept {
        return MemoryCounterSnapshot{
            counter.current_bytes.load(std::memory_order_relaxed),
            counter.peak_bytes.load(std::memory_order_relaxed),
            counter.denial_count.load(std::memory_order_relaxed),
            counter.denied_bytes.load(std::memory_order_relaxed),
        };
    }

    void note_denial(MemoryDomain domain, MemoryStage stage,
                     std::uint64_t bytes) noexcept {
        for (AtomicCounter* counter :
             {&total_, &counter_for(domain), &counter_for(stage)}) {
            saturating_add(counter->denial_count, 1);
            saturating_add(counter->denied_bytes, bytes);
        }
    }

    void release(std::uint64_t bytes, MemoryDomain domain,
                 MemoryStage stage) noexcept {
        counter_for(domain).current_bytes.fetch_sub(bytes,
                                                    std::memory_order_relaxed);
        counter_for(stage).current_bytes.fetch_sub(bytes,
                                                   std::memory_order_relaxed);
        total_.current_bytes.fetch_sub(bytes, std::memory_order_release);
    }

    std::atomic<std::uint64_t> limit_bytes_;
    AtomicCounter total_{};
    std::array<AtomicCounter, kMemoryDomainCount> domains_{};
    std::array<AtomicCounter, kMemoryStageCount> stages_{};

    friend class MemoryReservation;
};

inline void MemoryReservation::reset() noexcept {
    if (!budget_ || bytes_ == 0) return;
    MemoryBudget* const owner = budget_;
    const std::uint64_t released_bytes = bytes_;
    const MemoryDomain released_domain = domain_;
    const MemoryStage released_stage = stage_;
    budget_ = nullptr;
    bytes_ = 0;
    domain_ = MemoryDomain::Unknown;
    stage_ = MemoryStage::Unknown;
    owner->release(released_bytes, released_domain, released_stage);
}

// Constructed before the first process-global AllocationRegistry that requests
// it, so static destruction releases registry entries before this coordinator.
inline MemoryBudget& process_memory_budget() noexcept {
    static MemoryBudget budget;
    return budget;
}

}  // namespace spk::memory

#endif  // SPK_RUNTIME_MEMORY_BUDGET_H

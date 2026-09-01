#include "native_allocation_registry.h"
#include "raw_decoder.h"
#include "raw_decoder_jni_safety.h"
#include "raw_result_publication.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

int failures = 0;

constexpr std::uint32_t kDecodedFixtureBits[] = {
    0xbe800000U, 0x80000000U, 0x3f800000U, 0x7fc01234U};

void check(bool condition, const char* label) {
  std::cout << (condition ? "ok   " : "FAIL ") << label << '\n';
  if (!condition) ++failures;
}

struct PublicationProbe {
  sfraw::jni::PublicationDecision decision =
      sfraw::jni::PublicationDecision::Published;
  int cancelAtPoll = 0;
  int cancellationPolls = 0;
  int attempts = 0;
  int aborts = 0;
  bool throwDuringAttempt = false;
  sfraw::jni::NativePublication seen;
};

bool injectedCancellation(void* opaque) noexcept {
  auto& probe = *static_cast<PublicationProbe*>(opaque);
  ++probe.cancellationPolls;
  return probe.cancelAtPoll > 0 &&
      probe.cancellationPolls >= probe.cancelAtPoll;
}

sfraw::jni::PublicationDecision injectedPublication(
    const sfraw::jni::NativePublication& publication, void* opaque) {
  auto& probe = *static_cast<PublicationProbe*>(opaque);
  ++probe.attempts;
  probe.seen = publication;
  if (probe.throwDuringAttempt) {
    throw std::runtime_error("injected publication exception");
  }
  return probe.decision;
}

void injectedAbort(void* opaque) noexcept {
  ++static_cast<PublicationProbe*>(opaque)->aborts;
}

spectrafilm::DecodedFloatBuffer decodedFixture() {
  spectrafilm::DecodedFloatBuffer decoded;
  decoded.allocateUninitialized(4U);
  static_assert(sizeof(kDecodedFixtureBits) == 4U * sizeof(float));
  std::memcpy(decoded.data(), kDecodedFixtureBits,
              sizeof(kDecodedFixtureBits));
  return decoded;
}

}  // namespace

int main() {
  using sfraw::NativeAllocationRegistry;
  using sfraw::ReleaseResult;
  using sfraw::jni::checkedEncodedInputWindow;
  using sfraw::jni::PublicationDecision;
  using sfraw::jni::publishDecodedAllocation;

  std::uintptr_t resolved = 0U;
  const std::uintptr_t base = 0x1000U;
  check(checkedEncodedInputWindow(64, 0, 64, 64, 64, base, &resolved) &&
            resolved == base,
        "canonical direct-buffer window resolves to its base");
  check(!checkedEncodedInputWindow(64, 1, 64, 63, 64, base, &resolved),
        "non-canonical direct-buffer position is rejected at JNI boundary");
  check(!checkedEncodedInputWindow(64, 0, 63, 63, 64, base, &resolved),
        "non-canonical direct-buffer limit is rejected at JNI boundary");

  NativeAllocationRegistry registry;

  spectrafilm::DecodedFloatBuffer decoded;
  decoded.allocateUninitialized(4U);
  decoded[0] = -0.25F;
  decoded[1] = 0.0F;
  decoded[2] = 1.0F;
  decoded[3] = 2.0F;
  float* decodedBase = decoded.data();
  spectrafilm::DecodedFloatBuffer moved(std::move(decoded));
  check(decoded.empty() && moved.data() == decodedBase && moved.size() == 4U &&
            moved[0] == -0.25F && moved[3] == 2.0F,
        "decoded float storage moves without copying or changing bytes");
  void* handedOff = moved.release();
  check(moved.empty() && handedOff == decodedBase,
        "decoded float storage releases the exact malloc-compatible base");
  const std::uint64_t handoffId =
      registry.adopt(handedOff, 4U * sizeof(float));
  check(registry.release(handoffId, handedOff, 4U * sizeof(float)) ==
            ReleaseResult::Released,
        "released decode storage is adopted and freed exactly once");

  bool overflowRejected = false;
  try {
    decoded.allocateUninitialized(
        std::numeric_limits<std::size_t>::max() / sizeof(float) + 1U);
  } catch (const std::bad_alloc&) {
    overflowRejected = true;
  }
  check(overflowRejected && decoded.empty(),
        "decoded float allocation overflow fails closed without ownership");

  {
    auto candidate = decodedFixture();
    void* const exactBase = candidate.data();
    PublicationProbe probe;
    probe.cancelAtPoll = 1;
    const auto outcome = publishDecodedAllocation(
        candidate, 4U * sizeof(float), registry, injectedCancellation, &probe,
        injectedPublication, &probe, injectedAbort);
    check(outcome.decision == PublicationDecision::Cancelled &&
              candidate.data() == exactBase && candidate.size() == 4U &&
              registry.outstanding() == 0U && probe.attempts == 0 &&
              probe.aborts == 0,
          "pre-handoff cancellation preserves the decode buffer owner");
  }

  {
    auto candidate = decodedFixture();
    void* const exactBase = candidate.data();
    PublicationProbe probe;
    const auto outcome = publishDecodedAllocation(
        candidate, 4U * sizeof(float), registry, injectedCancellation, &probe,
        injectedPublication, &probe, injectedAbort);
    check(outcome.decision == PublicationDecision::Published &&
              candidate.empty() && outcome.publication.base == exactBase &&
              outcome.publication.capacity == 4U * sizeof(float) &&
              outcome.publication.token != 0U && probe.attempts == 1 &&
              probe.aborts == 0 && registry.outstanding() == 1U &&
              std::memcmp(outcome.publication.base, kDecodedFixtureBits,
                          sizeof(kDecodedFixtureBits)) == 0,
          "successful production publication transfers the exact bytes once");
    check(registry.release(outcome.publication.token,
                           outcome.publication.base,
                           outcome.publication.capacity) ==
                  ReleaseResult::Released &&
              registry.outstanding() == 0U,
          "published result close frees its sole registry owner");
  }

  {
    auto candidate = decodedFixture();
    PublicationProbe probe;
    probe.decision = PublicationDecision::Failed;
    const auto outcome = publishDecodedAllocation(
        candidate, 4U * sizeof(float), registry, injectedCancellation, &probe,
        injectedPublication, &probe, injectedAbort);
    check(outcome.decision == PublicationDecision::Failed &&
              candidate.empty() && probe.attempts == 1 && probe.aborts == 1 &&
              registry.outstanding() == 0U &&
              registry.release(outcome.publication.token,
                               outcome.publication.base,
                               outcome.publication.capacity) ==
                  ReleaseResult::UnknownToken,
          "post-adopt publication failure rolls back exactly once");
  }

  {
    auto candidate = decodedFixture();
    PublicationProbe probe;
    probe.cancelAtPoll = 2;
    const auto outcome = publishDecodedAllocation(
        candidate, 4U * sizeof(float), registry, injectedCancellation, &probe,
        injectedPublication, &probe, injectedAbort);
    check(outcome.decision == PublicationDecision::Cancelled &&
              candidate.empty() && probe.attempts == 0 && probe.aborts == 1 &&
              registry.outstanding() == 0U &&
              registry.release(outcome.publication.token,
                               outcome.publication.base,
                               outcome.publication.capacity) ==
                  ReleaseResult::UnknownToken,
          "post-adopt cancellation rolls back before platform publication");
  }

  {
    auto candidate = decodedFixture();
    PublicationProbe probe;
    probe.cancelAtPoll = 3;
    const auto outcome = publishDecodedAllocation(
        candidate, 4U * sizeof(float), registry, injectedCancellation, &probe,
        injectedPublication, &probe, injectedAbort);
    check(outcome.decision == PublicationDecision::Cancelled &&
              candidate.empty() && probe.attempts == 1 && probe.aborts == 1 &&
              registry.outstanding() == 0U &&
              registry.release(outcome.publication.token,
                               outcome.publication.base,
                               outcome.publication.capacity) ==
                  ReleaseResult::UnknownToken,
          "post-publication cancellation revokes Java ownership exactly once");
  }

  {
    auto candidate = decodedFixture();
    PublicationProbe probe;
    probe.throwDuringAttempt = true;
    bool threw = false;
    sfraw::jni::NativePublication attempted;
    try {
      (void)publishDecodedAllocation(
          candidate, 4U * sizeof(float), registry, injectedCancellation,
          &probe, injectedPublication, &probe, injectedAbort);
    } catch (const std::runtime_error&) {
      threw = true;
      attempted = probe.seen;
    }
    check(threw && candidate.empty() && probe.attempts == 1 &&
              probe.aborts == 1 && registry.outstanding() == 0U &&
              registry.release(attempted.token, attempted.base,
                               attempted.capacity) ==
                  ReleaseResult::UnknownToken,
          "publication exception rolls back and leaves no outstanding entry");
  }

  void* owned = std::malloc(64U);
  check(owned != nullptr, "fixture allocation succeeds");
  if (owned == nullptr) return 1;

  const std::uint64_t id = registry.adopt(owned, 64U);
  check(id != 0U && registry.outstanding() == 1U,
        "adopt assigns a nonzero generation and records ownership");

  void* foreign = std::malloc(64U);
  check(foreign != nullptr, "foreign fixture allocation succeeds");
  if (foreign != nullptr) {
    check(registry.release(id, foreign, 64U) == ReleaseResult::Mismatch,
          "foreign base is rejected without releasing the owner");
    std::free(foreign);
  }
  check(registry.release(id, static_cast<unsigned char*>(owned) + 1, 63U) ==
            ReleaseResult::Mismatch,
        "interior alias is rejected without releasing the owner");
  check(registry.release(id, owned, 63U) == ReleaseResult::Mismatch,
        "capacity mismatch is rejected without releasing the owner");

  std::atomic<int> released{0};
  std::atomic<int> stale{0};
  std::vector<std::thread> racers;
  racers.reserve(32U);
  for (int i = 0; i < 32; ++i) {
    racers.emplace_back([&] {
      const ReleaseResult outcome = registry.release(id, owned, 64U);
      if (outcome == ReleaseResult::Released) ++released;
      if (outcome == ReleaseResult::UnknownToken) ++stale;
    });
  }
  for (auto& racer : racers) racer.join();

  check(released.load() == 1 && stale.load() == 31,
        "concurrent repeated release frees exactly once");
  check(registry.outstanding() == 0U,
        "successful release removes the registry entry");
  check(registry.release(id, owned, 64U) == ReleaseResult::UnknownToken,
        "sequential double release is a safe stale-token rejection");

  sfraw::NativeCancellationRegistry cancellations;
  const std::uint64_t cancellationId = cancellations.create();
  const auto cancellationLease = cancellations.acquire(cancellationId);
  check(cancellationId != 0U && cancellationLease != nullptr &&
            !cancellationLease->load(),
        "cancellation token begins live and not cancelled");
  check(cancellations.cancel(cancellationId) && cancellationLease->load(),
        "cancel is immediately visible through an acquired decode lease");
  check(cancellations.release(cancellationId) &&
            cancellations.acquire(cancellationId) == nullptr &&
            !cancellations.cancel(cancellationId),
        "released cancellation generation becomes a safe stale token");

  const std::uint64_t closeId = cancellations.create();
  const auto inFlightLease = cancellations.acquire(closeId);
  check(cancellations.release(closeId) && inFlightLease != nullptr &&
            inFlightLease->load(),
        "closing a token cancels an already acquired in-flight lease");
  check(cancellations.outstanding() == 0U,
        "cancellation registry has no leaked generations");

  std::cout << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED")
            << " (" << failures << " failures)\n";
  return failures == 0 ? 0 : 1;
}

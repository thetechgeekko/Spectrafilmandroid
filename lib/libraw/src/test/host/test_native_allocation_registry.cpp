#include "native_allocation_registry.h"
#include "raw_decoder_jni_safety.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* label) {
  std::cout << (condition ? "ok   " : "FAIL ") << label << '\n';
  if (!condition) ++failures;
}

}  // namespace

int main() {
  using sfraw::NativeAllocationRegistry;
  using sfraw::ReleaseResult;
  using sfraw::jni::checkedEncodedInputWindow;
  using sfraw::jni::copyBytesCancellable;

  std::uintptr_t resolved = 0U;
  const std::uintptr_t base = 0x1000U;
  check(checkedEncodedInputWindow(64, 0, 64, 64, 64, base, &resolved) &&
            resolved == base,
        "canonical direct-buffer window resolves to its base");
  check(!checkedEncodedInputWindow(64, 1, 64, 63, 64, base, &resolved),
        "non-canonical direct-buffer position is rejected at JNI boundary");
  check(!checkedEncodedInputWindow(64, 0, 63, 63, 64, base, &resolved),
        "non-canonical direct-buffer limit is rejected at JNI boundary");

  constexpr std::size_t kChunk = 128U;
  std::vector<unsigned char> copySource(kChunk * 2U + 3U, 0x5aU);
  std::vector<unsigned char> copyDestination(copySource.size(), 0U);
  struct CopyCancellation {
    int polls = 0;
  } copyCancellation;
  auto cancelOnSecondPoll = [](void* opaque) noexcept -> bool {
    return ++static_cast<CopyCancellation*>(opaque)->polls >= 2;
  };
  check(!copyBytesCancellable(copyDestination.data(), copySource.data(),
                              copySource.size(), cancelOnSecondPoll,
                              &copyCancellation, kChunk) &&
            copyCancellation.polls == 2 &&
            std::equal(copySource.begin(), copySource.begin() + kChunk,
                       copyDestination.begin()) &&
            std::all_of(copyDestination.begin() + kChunk,
                        copyDestination.end(),
                        [](unsigned char value) { return value == 0U; }),
        "RAW result transfer stops cooperatively before stale publication");

  NativeAllocationRegistry registry;
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

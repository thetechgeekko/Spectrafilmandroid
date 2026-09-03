/*
 * Spektrafilm for Android -- decoded RAW ownership publication seam.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
#include "raw_result_publication.h"

#include <cstdlib>
#include <limits>

namespace sfraw::jni {
namespace {

bool cancellationRequested(PublicationCancellationCheck cancellation,
                           void* context) noexcept {
  return cancellation != nullptr && cancellation(context);
}

}  // namespace

PublicationOutcome publishDecodedAllocation(
    spectrafilm::DecodedFloatBuffer& decoded, std::size_t byteCount,
    NativeAllocationRegistry& registry,
    PublicationCancellationCheck cancellation, void* cancellationContext,
    PublicationAttempt attemptPublication, void* publicationContext,
    PublicationAbort abortPublication) {
  PublicationOutcome outcome;
  if (decoded.data() == nullptr || decoded.empty() || byteCount == 0U ||
      decoded.size() >
          std::numeric_limits<std::size_t>::max() / sizeof(float) ||
      decoded.size() * sizeof(float) != byteCount) {
    return outcome;
  }
  if (cancellationRequested(cancellation, cancellationContext)) {
    outcome.decision = PublicationDecision::Cancelled;
    return outcome;
  }

  outcome.publication.base = decoded.release();
  outcome.publication.capacity = byteCount;
  try {
    outcome.publication.token =
        registry.adopt(outcome.publication.base, byteCount);
  } catch (...) {
    std::free(outcome.publication.base);
    outcome.publication.base = nullptr;
    outcome.publication.capacity = 0U;
    throw;
  }

  auto rollback = [&]() noexcept {
    if (abortPublication != nullptr) abortPublication(publicationContext);
    (void)registry.release(outcome.publication.token,
                           outcome.publication.base,
                           outcome.publication.capacity);
  };

  if (cancellationRequested(cancellation, cancellationContext)) {
    outcome.decision = PublicationDecision::Cancelled;
    rollback();
    return outcome;
  }

  try {
    outcome.decision = attemptPublication != nullptr
        ? attemptPublication(outcome.publication, publicationContext)
        : PublicationDecision::Failed;
  } catch (...) {
    rollback();
    throw;
  }

  if (outcome.decision == PublicationDecision::Published &&
      cancellationRequested(cancellation, cancellationContext)) {
    outcome.decision = PublicationDecision::Cancelled;
  }
  if (outcome.decision != PublicationDecision::Published) rollback();
  return outcome;
}

}  // namespace sfraw::jni

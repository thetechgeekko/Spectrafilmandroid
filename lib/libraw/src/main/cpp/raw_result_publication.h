/*
 * Spektrafilm for Android -- decoded RAW ownership publication seam.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
#ifndef SFRAW_RAW_RESULT_PUBLICATION_H
#define SFRAW_RAW_RESULT_PUBLICATION_H

#include <cstddef>
#include <cstdint>

#include "native_allocation_registry.h"
#include "raw_decoder.h"

namespace sfraw::jni {

struct NativePublication {
  void* base = nullptr;
  std::size_t capacity = 0U;
  std::uint64_t token = 0U;
};

enum class PublicationDecision {
  Published,
  Failed,
  Cancelled,
};

// Polling must never unwind across an ownership transition; cancellation is a
// value result and all exceptional platform publication uses the attempt path.
using PublicationCancellationCheck = bool (*)(void* context) noexcept;
using PublicationAttempt = PublicationDecision (*)(
    const NativePublication& publication, void* context);
using PublicationAbort = void (*)(void* context) noexcept;

struct PublicationOutcome {
  PublicationDecision decision = PublicationDecision::Failed;
  // Retained even after rollback so tests/diagnostics can prove the generation
  // is stale. The base must never be dereferenced unless decision is Published.
  NativePublication publication;
};

/**
 * Transfer one fully initialized decode buffer into a registry and attempt its
 * platform publication.
 *
 * Before release(), the DecodedFloatBuffer remains the owner. After adopt(),
 * every failure, exception, or cancellation invokes abortPublication and
 * releases the exact token/base/capacity once. A Published outcome intentionally
 * leaves the registry as owner for the public result's later close().
 */
PublicationOutcome publishDecodedAllocation(
    spectrafilm::DecodedFloatBuffer& decoded, std::size_t byteCount,
    NativeAllocationRegistry& registry,
    PublicationCancellationCheck cancellation, void* cancellationContext,
    PublicationAttempt attemptPublication, void* publicationContext,
    PublicationAbort abortPublication);

}  // namespace sfraw::jni

#endif  // SFRAW_RAW_RESULT_PUBLICATION_H

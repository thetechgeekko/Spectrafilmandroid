# LibRaw 0.22.2 patch manifest

<!-- libraw-license-route: UNRESOLVED -->

LibRaw Android distribution route: UNRESOLVED.

The Android and host builds apply this ordered series to the official
`LibRaw-0.22.2.tar.gz` release. `cmake/LibRawVendor.cmake` verifies every patch
SHA-256, applies it idempotently, and then checks the resulting source contracts.

Upstream baseline:

- URL: `https://www.libraw.org/data/LibRaw-0.22.2.tar.gz`
- archive SHA-256: `de86b035655accff8d4010f1a221fdf50d353cb7b1422ba26f14a0db92612cfa`
- archive size: `1,682,962` bytes
- annotated tag object: `24fa7e5463cbf8b8615dbd2b16c933a294d52400`
- peeled commit: `b93f6e45c194f5df9b02a43b1af9a54b4f41f33f`
- tag signature: none; the archive hash is therefore the shipping trust anchor
- audited patched-tree aggregate: `bc463c30e414781d2455a47b99c30741830798b5941a049782b560fbb3abc74c`
  over the resolver's sorted 100-file source/header manifest

Patch order:

1. `0001-openmp-wavelet-initialize-size.patch`
   (`b73a81a79a918d76ffb29f5272cac877cd31e4c1c6d039fe70b32d31ccfb4de6`)
   restores the `size = iheight * iwidth` initialization accidentally removed
   from the OpenMP branch. Tracks upstream LibRaw issue #842 and regression
   commit `d932fa118489928a6d33d648644297e530f2036e`.
2. `0002-newsubfiletype-unsigned.patch`
   (`84c77a94b904944340c8921e6b8f47600fa733a30376f08207e3376b06a8e7cd`)
   is the source portion of upstream PR #853 / commit
   `d9437df806524dbd9be4354b1ce83d5b479acafa`. It removes the file-controlled
   float-to-signed-int conversion reported by issue #844 while preserving the
   full unsigned TIFF tag value.
3. `0003-ljpeg-zero-category.patch`
   (`4330ad0c8ef7a996cbbb75a227c1c71c98c7f2e46986bcac98e02660bb8e4010`)
   handles JPEG lossless Huffman category zero as the standards-defined zero
   difference and rejects categories outside `0..16` before shifting. LibRaw
   issues #367 and #473 document the otherwise reachable UBSan shift; upstream
   closed them as damaged-input/GIGO, so this is a Spektrafilm hardening patch.
4. `0004-bound-tiff-metadata-allocations.patch`
   (`4546199834ebeb8cbfbd2ad3613afff7a7f8e9fa8bf38be6994a5688868e2caf`)
   adds a compile-time cumulative allocation ceiling for LibRaw's identify
   phase and rejects duplicate allocation-bearing TIFF strip/XMP/DNG-opcode
   tags. Spektrafilm sets the ceiling to 16 MiB on Android and in the sanitizer
   harness, preventing a small shared TIFF payload from being allocated
   hundreds of times before the wrapper can inspect dimensions.
5. `0005-bound-lossless-jpeg-work.patch`
   (`d10a59f884e652c3764e793c2a5ebcc56760c07538f1fbd6a0bdefdd760abdb0`)
   rejects an embedded SOF3 stream whose decoded sample count exceeds both a
   bounded geometry allowance and LibRaw's configured raw-memory budget. This
   prevents tiny category-zero entropy from amplifying into hundreds of millions
   of generic or tiled-DNG lossless-JPEG loop iterations while invalid destination
   stores are skipped.
6. `0006-bound-dng-tile-streams.patch`
   (`86221caa689460fbd04e25ef93cc3d87b7450b17b6b5843a28c4b984994ac4c2`)
   bounds DNG tile-stream count and cumulative lossless-JPEG sample work.
7. `0007-bound-sony-ljpeg-work.patch`
   (`45d4735a5e725deabd414f8630db91587dac09329011d546ecd307fe329b1274`)
   validates Sony tiled-LJPEG geometry and bounds tile/setup work.
8. `0008-bound-ljpeg-segments.patch`
   (`bcbedeefb69b864ae888040b4ae31174e95dcc39e6800852acdb5d7192f3bd39`)
   makes marker parsing exact, allocation-checked, and work-bounded.
9. `0009-bound-ljpeg-setup-work.patch`
   (`54bfc7b45935f23dc92fa7035236358c5c39b63ea1c82d4c626fd7d95a7d3a5a`)
   accounts Huffman/quantization setup work across each LJPEG stream.
10. `0010-harden-hasselblad-ljpeg.patch`
    (`3aaf0f17318367096e13e0c3436f095e025c6555b75a5537416fd42830c2b378`)
    rejects invalid Hasselblad geometry, categories, and pixel stores.
11. `0011-harden-ljpeg-idct.patch`
    (`b7a267feecd93ab98bc52f1d4f7e1d2901ca500059b01e89eee46b03077df7e6`)
    checks lossless-JPEG predictor/IDCT arithmetic before narrowing.
12. `0012-harden-cr2-slice-arithmetic.patch`
    (`977105fb62a9ccfe6f49295fa79fee33c4aeb2273b90df8ffab617e95d3f7b82`)
    validates CR2 slice geometry and performs index arithmetic in wide types.
13. `0013-harden-canon-sraw.patch`
    (`423eb01e16c51d72fccd8284dd063ea69dea38b0ac8aed255bea37f1875f6575`)
    validates Canon sRAW rows, slices, components, and signed arithmetic.
14. `0014-bound-identify-ljpeg-work.patch`
    (`613a2039f9d97ea3e0a7c915f233f1a9b5cf23134b612a73a5c1b4597b9db31a`)
    applies a cumulative LJPEG setup budget during identify/probe paths.
15. `0015-bound-fp-dng-compressed-work.patch`
    (`03947dd32d72c2e578c7d727ef5d2a74af9f7e340245e56b883f127aeafb3395`)
    validates compressed floating-point DNG offsets, sizes, and cumulative work.
16. `0016-bound-canon-sraw-white-balance.patch`
    (`a0333dec464b105c45b1802a742e95582704efcba8c15e7f0d6871153dd93546`)
    rejects non-finite or out-of-range Canon sRAW white-balance scaling.
17. `0017-make-ljpeg-idct-init-thread-safe.patch`
    (`7ac3ae1e6c36d7f862026e5be036d1456fdf6b87b4eac3f920022c42c13c7522`)
    replaces racy first-use IDCT table writes with immutable initialization.
18. `0018-bound-identify-maximum-shift.patch`
    (`994cbbb5988f66fd9331550cc99bfe1b9ddc92db20c99a32d32dcccf7bb7bc18`)
    defines maximum/black-level shifts for float and high-bit TIFF metadata.
19. `0019-bound-hasselblad-predictor-arithmetic.patch`
    (`088744bd7d349b0fbc0eecfc7933aad3f0cac49f95e2a0d48c8896a758f740c7`)
    widens and range-checks Hasselblad predictor accumulation.
20. `0020-bound-olympus-metadata-and-arithmetic.patch`
    (`b28ebf2e312a09cbf7b2b6969b289cee7b6dd74ec67ea70ee29ca531cc61bcf2`)
    validates Olympus 14-bit metadata, exact refills, unary work, shifts,
    predictors, and pixel narrowing.
21. `0021-harden-panasonic-c8-decoder.patch`
    (`d528a6399ddcc9e14e07a0d76c9bc01523ce0547ff81f2b8d927908d234da36b`)
    preserves raw C8 table counts, validates table/stripe/in-file-range/bit
    budgets, and uses defined predictor arithmetic with an OpenMP-safe error
    reduction.
22. `0022-bound-fixed-header-string-reads.patch`
    (`29fd07f40f82d34b1ffafd55f51e277d30ff3ecf22597c2d30dc4c45676695cf`)
    preserves the required NUL in legacy identify signatures, reserves a
    MakerNote sentinel byte, and bounds dormant X3F model searches/copies by the
    actual 2048-byte probe result. The X3F parser is disabled in Android today;
    the host sanitizer harness enables it to prevent a future configuration
    switch from exposing the latent boundary reads.
23. `0023-record-local-modification-notices.patch`
    (`3d8d2eef4f59ef665d58fede9ae780d523deb5a119c6c67ed6844ad4812edc38`)
    records the 2026-08-30 Spektrafilm Android modification date and contributor
    attribution in each of the 17 upstream files changed by patches 0001–0022.
    It changes notices only; the preceding aggregate remains a recognized
    migration input so a clean chain can apply this notice patch exactly.
24. `0024-define-xtrans-negative-index-arithmetic.patch`
    (`71b8b6810416f407549a52c3b21cb91720d6935dee1da8adfbd1ef50e53e138a`)
    preserves the X-Trans interpolator's intended negative neighbor offsets as
    `-(i << c)`. This removes undefined left shifts of negative integers without
    changing the selected pixels or the exact processed-image digest.
25. `0025-define-icc-s15fixed16-conversion.patch`
    (`895b644886247bb3853002c27aa5d56475429c05f62ec556360a036dc8f3a9a4`)
    rounds generated ICC XYZ matrix entries in the signed domain before their
    defined modulo encoding as 32-bit s15Fixed16 words. This preserves negative
    ACES profile coefficients without undefined floating-to-unsigned conversion.

Patch 0021 deliberately preserves ordered first-match decoding rather than
requiring a prefix-free codebook: Panasonic S5M2 metadata contains an 8-bit
entry that shadows two 12-bit entries. An unmatched `huff_index == 17` is
rejected fail-closed instead of using upstream's implicit zero fallback. Captured
GH6, GH7, G9M2, and S5M2 tables cover every input prefix, so no known valid table
depends on that fallback; real-camera corpus qualification remains a release
check when new Panasonic models appear.

The 0.22.2 baseline already includes the CR2Slice column bound from
CVE-2026-21413 / TALOS-2026-2331 (stable fix `75ed2c12a35b765b3b6ad695cc1f044f19efe644`).
The resolver verifies that guard and deliberately carries no duplicate patch.

LibRaw is offered under LGPL-2.1-only or CDDL-1.0. The repository has not selected
a distribution route for the static integration. The `UNRESOLVED` marker is a
release-blocking state; including both upstream texts is provenance only. Local
patches, corresponding source, relink recipe, notices, and SBOM are assembled by
the route-neutral compliance tooling, but a green verifier is not legal approval.
The canonical decision record deliberately leaves patch-contribution license and
rights confirmation unset until an authorized human records both with an HTTPS
approval reference. Ticket #166 owns that decision and the release gate.

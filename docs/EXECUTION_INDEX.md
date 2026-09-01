# Spektrafilm execution index

Status: **canonical navigation and execution protocol**. Last reconciled 2026-09-01.

This page answers one question: *where is the current truth?* It deliberately does not copy the
open-work queue. GitHub's native issue state, dependencies, labels, and assignees are the live
queue; repository documents explain the contracts and retain evidence.

## Authority order

When two documents disagree, use this order:

1. The [Wayfinder map: production-ready Spektrafilm + 1–2 s exact export](https://github.com/thetechgeekko/Spektrafilm-android/issues/164)
   and its native child/dependency graph own live status, priority, blockers, and claims. Its nested
   [Wayfinder workstream: 1–2 s exact export + fast interactive preview](https://github.com/thetechgeekko/Spektrafilm-android/issues/117) owns the
   exact-export and interactive-preview implementation sequence.
2. [PRODUCTION_READINESS_PLAN.md](PRODUCTION_READINESS_PLAN.md) owns release acceptance and the
   implementation architecture. [BIT_IDENTICAL_EXPORT_ROADMAP.md](BIT_IDENTICAL_EXPORT_ROADMAP.md)
   owns numeric contracts, performance strategy, measurements, and dependency decisions.
3. [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) owns the immutable candidate, signing, legal,
   device, and publication procedure. It is not a backlog.
4. Domain contracts own their narrow surface: [LICENSING.md](LICENSING.md),
   [JNI_LIFETIME_SAFETY.md](JNI_LIFETIME_SAFETY.md),
   [TRANSACTIONAL_STORAGE.md](TRANSACTIONAL_STORAGE.md), [RAW_DNG.md](RAW_DNG.md), and
   [MASK_JSON_SCHEMA.md](MASK_JSON_SCHEMA.md).
5. `docs/research/**`, [AUDIT.md](AUDIT.md), [ROADMAP.md](ROADMAP.md),
   [PERF_ROADMAP.md](PERF_ROADMAP.md), [EXPORT_FASTPATH.md](EXPORT_FASTPATH.md), and
   [HANDOFF.md](../HANDOFF.md) retain findings and history. They do not own current issue state or
   execution order.

Build files, workflows, and tests outrank prose for facts about the current tree. If prose and code
disagree, fix the prose or open a ticket; do not silently reinterpret the implementation.

## Fast execution loop

From the repository root:

Prerequisites are Python 3.10+ and an installed, authenticated GitHub CLI (`gh auth status`) with
read access to the repository. The native sub-issue order is the map's priority order; changing that
order is a deliberate planning mutation.

```powershell
python tools/wayfinder/frontier.py --strict
```

The command recursively reads both live maps and separates unblocked agent work, human decisions,
claimed work, and blocked work. It also reports routing hygiene errors. Work in this order:

1. Select the first unassigned, unblocked `ready-for-agent` ticket whose files do not overlap
   another active claim.
2. Claim it with `gh issue edit <number> --add-assignee '@me'` before the first repository write.
3. Read the ticket acceptance criteria and only the relevant domain contracts/research.
4. Implement, run the ticket's narrow tests, then run every affected release/parity/device gate.
5. Commit one coherent slice. A local SHA is branch-local evidence until that commit is pushed.
6. Resolve only when every acceptance item has evidence: comment with answer/tests/commit, close the
   ticket, and append one short resolution pointer to the parent map. Otherwise post a handoff and
   release the claim.
7. Run the frontier command again. Native dependencies, not a handwritten checklist, choose the
   next ticket.

Use parallel agents only for non-overlapping frontier tickets. Human-decision tickets remain
`ready-for-human`; do not guess product, legal, distribution, or exactness policy.

## Current tree facts

These values describe this branch. The offline checker derives and verifies version/SDK/build-tools,
parity count, common inline/reference links and local images, policy markers, and preset IDs; the remaining rows name their
executable source for direct review:

| Fact | Current value | Executable source |
|---|---|---|
| App version | `0.9.0` / versionCode `11` | `app/build.gradle.kts` |
| Android SDK | min 24, target 34, compile 34 | Gradle module build files |
| Build system | AGP `8.7.3`, Kotlin `2.0.21`, Gradle `8.14.3` | version catalog + wrapper properties |
| Native toolchain | NDK `27.0.12077973`, CMake `3.22.1` | Gradle + CI/release workflows |
| Android build tools | `35.0.0` | app Gradle + CI/release workflows |
| RAW decoder | authenticated, patched LibRaw `0.22.2`; X3F disabled pending qualification | `LibRawVendor.cmake` + domain record |
| Engine gate | 39 cases at O2 and shipping `-O3 -ffast-math -fno-finite-math-only` | `.github/workflows/ci.yml` |
| Public release | `v0.9.0`; current tree remains under release hold | release checklist + live production map |

Target/compile SDK 34 is current-tree truth, not release approval. The owner has selected a free
GitHub release followed by a paid Google Play supporter channel that unlocks nothing. API-36 and
Android behavior qualification remain live in [#171](https://github.com/thetechgeekko/Spektrafilm-android/issues/171),
and the final Play publication contract lives in [#200](https://github.com/thetechgeekko/Spektrafilm-android/issues/200).
The two channels must represent the same logical release; APK/AAB signing, ZIP layout, and
device-split bytes are reported separately. Likewise, a locally device-qualified debug-key
candidate is not production-signing evidence.

## Product and numeric contracts

- **Strict Exact CPU** is the parity-bearing route: oracle tolerance (`max_abs <= 1e-4`,
  `rms <= 1e-5`) plus byte identity across worker counts for the same build. Cross-build,
  cross-ABI, CPU/GPU, and whole-container byte identity are not implied.
- **Fast GPU** is a separate, capability-gated Vulkan route: oracle-equivalent and same-device
  deterministic with CPU fallback. It must never be described as Strict Exact CPU or universal
  byte identity.
- The Android/native boundary uses linear float32 buffers. The strict CPU implementation retains
  its adopted mixed-precision arithmetic, including float64 where required; a whole-pipeline f16 or
  f32 rewrite would be a new numeric contract. Vulkan uses float32. Float16 is an optional measured
  optimization only after an explicit quality gate, never a default assumption.
- Sensor precision, processing precision, and file encoding depth are different contracts. The app
  can write 16-bit integer and 32-bit-float files. Current-source preservation is tracked by
  [Preserve declared native RAW sample precision through linear conversion](https://github.com/thetechgeekko/Spektrafilm-android/issues/190);
  future official Android 17 capability research is standalone
  [Track official Android 17 RAW14 capability and design a gated adapter](https://github.com/thetechgeekko/Spektrafilm-android/issues/193); an honest HDR gain-map contract is separate again.
- The 1–2 second promise is not yet a universal cold-render claim. The credible strict perceived
  route is idle full-resolution pre-render plus a content-addressed cache; cold Fast GPU and cold
  Strict CPU need their own measured rows in the owner-approved SLO matrix.
- Keep patched LibRaw for the qualified production decoder. Do not wholesale-adopt RawSpeed, Adobe
  DNG SDK, vkdt, Halide, or another library without the per-stage benchmark, format coverage,
  licensing, memory, APK-size, and numeric gates in the performance plan. Reuse proven techniques
  where they win; do not replace the app architecture by name.
- [#199](https://github.com/thetechgeekko/Spektrafilm-android/issues/199) owns the final spectral
  documentation product: an HonKit-style GitHub Pages portal for Spektrafilm Android and explicitly
  allowlisted public LATENT material, plus an offline Android viewer and one canonical user-facing
  License & Attribution document. LATENT stays private-by-default and is never bulk-published.
- [#200](https://github.com/thetechgeekko/Spektrafilm-android/issues/200) is the post-GitHub-release
  distribution tail. The paid Play listing is a supporter contribution, not a feature entitlement.

## Documentation classes

| Class | Meaning | Examples |
|---|---|---|
| Canonical execution | Current policy and implementation contracts | this index, production plan, exact-export roadmap |
| Release procedure | Required steps for one immutable candidate | release checklist, licensing and safety contracts |
| Shipped reference | Describes an implemented format or subsystem | assets, stocks, presets, RAW/DNG, masking schema |
| Finding inventory | May contain open and resolved findings; live state stays in GitHub | audit, improvement backlog |
| Historical plan/lab | Preserved evidence; never the current queue | old roadmaps, fast-path history, handoff, device reports |
| Research/decision input | Evidence and alternatives; only an accepted ticket/map decision changes policy | `docs/research/**`, Lightroom studies |

## Reconciliation gate

Before updating a ticket or handing work to another agent, run:

```powershell
python tools/docs/check_docs_consistency.py
python tools/wayfinder/frontier.py --strict
git diff --check
```

Performance claims must name the source commit/artifact, device and OS, workload and dimensions,
route/effects/format, cold or warm state, sample count, statistic (at least median or p50/p95), and
uncertainty. A historical result without those fields stays historical and cannot satisfy a release
SLO.

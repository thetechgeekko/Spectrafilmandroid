# Mask compositor memory contract (ticket #141)

## Closure boundary

The mask compositor's bounded-memory implementation and exact-valid-input oracle are host-verified.
The production route no longer retains full-frame alpha, luma, or blur scratch planes; it preserves
the legacy float32 result exactly and admits all managed scratch through `JvmMemoryReservation` at
`MemoryBudgetStage.SPATIAL`. Current-source release-device requalification remains pending after the
live-cancellation and evidence-provenance changes described below.

This does **not** close the product's 1-2 second export-performance target. In the historical phone run,
four stacked masks with all current spatial controls took about 15.5 seconds at 12.5 MP and 62 seconds
at 50 MP. Reaching 1-2 seconds requires a later accelerated native/GPU pipeline, not another claim
against this Kotlin implementation.

## Why a halo tile alone is insufficient

Each spatial control uses three separable box passes in this order:

`H1 -> V1 -> H2 -> V2 -> H3 -> V3`

The mathematical support is `3 * radius` on every side. A one-radius halo produces seams. A
three-radius halo is mathematically sufficient, but restarting a float32 rolling sum at every tile edge
changes accumulation order and breaks byte identity.

The production implementation is therefore a row stream:

- every horizontal pass retains the legacy left-to-right recurrence;
- every vertical column retains the legacy top-to-bottom recurrence;
- three radius-bounded vertical rings retain outgoing rows for `V1`, `V2`, and `V3`;
- all active radii run concurrently;
- a radius `r` waits in a bounded output-delay ring until the largest radius `R` reaches the same
  global row;
- mask geometry is rasterized into one reusable alpha row; and
- adjustments are still committed in document order.

This preserves global-edge clamping, all six pass orders, stacked-mask order, and the existing
pointwise/spatial blend law.

## Checked admission and ownership

Before the first output mutation, the compositor validates:

- positive dimensions;
- checked `width * height * 3 * sizeof(float)` arithmetic;
- the JVM-indexable pixel and array extents;
- a writable buffer; and
- sufficient bytes between the caller's current position and limit.

A non-zero aligned buffer position is a supported image origin. Prefix/suffix bytes and the caller's
position, limit, and byte order remain unchanged.

Scratch admission occurs before the first float-array allocation. Denial allocates no scratch and
does not release a token that was never issued. An allocation failure after admission releases the
token once. Normal cleanup is idempotent and releases exactly once. Host tests inject each of these
paths and verify unchanged pixels and reservation diagnostics.

## Radius-aware memory bound

For width `W`, height `H`, each active unique positive radius `r`, and the largest radius `R` used by
the same adjustment:

```text
shared floats = 2W + 3
per-radius floats = W * [3*min(H, 2r+1) + 3 + 6 + 3(R-r) + 1]
payload bytes = 4 * (shared + sum(per-radius))
reservation = payload + checked array/owner accounting
```

The per-radius terms are three vertical rings, three vertical-sum rows, six pipeline rows, and the
exact-order output-delay ring. Scratch is reused across masks, so mask count changes run time but not
the peak reservation.

| Cell | Dimensions | Pixels | Integer radii | Float-plane bytes | Payload | Reserved | Scratch / plane |
|---|---:|---:|---:|---:|---:|---:|---:|
| 12.5 MP | 4096 x 3052 | 12,500,992 | 6, 24, 122, 204 | 50,003,968 B | 58,490,892 B | 58,496,748 B | 1.170x |
| 50 MP | 8192 x 6104 | 50,003,968 | 12, 49, 245, 409 | 200,015,872 B | 232,882,188 B | 232,888,044 B | 1.164x |

The earlier full-frame implementation could retain roughly seven float planes: alpha, luma, four
blur outputs, and another blur copy/temporary. The bounded implementation stays below two float
planes for the approved cells. The RGB image itself is separate: 150,011,904 bytes at 12.5 MP and
600,047,616 bytes at 50 MP.

## Cancellation and publication transaction

The bounded seam polls admission, raster, luma, all six filter passes, and composition progress. The
production overload accepts the engine's live `RenderCancellation` token; `EngineHelpers` can carry
that token through both bitmap-grading entry points. Scratch ownership remains exact: success,
allocation failure, and cancellation each release an admitted reservation exactly once.

Admission cancellation occurs before output mutation and is pixel-atomic. Once work has begun,
cancellation may stop after completed rows or filter-pass work. That intentionally favors prompt
cancellation over a full-frame rollback: the caller must perform cancellable grading only in a private,
unpublished scratch buffer and discard it on cancellation. Unit tests request cancellation after real
progress at every work stage, assert the precise observed stage, and verify exact-once cleanup. The
remaining integration seam is `MainActivity`: its preview, bitmap export, and high-bit export call
sites still need to create/pass the live token and enforce discard-on-cancel before release closure.

## Frozen oracle and adversarial coverage

`MaskTiledCompositorTest` contains an independent frozen copy of the pre-#141 compositor and blur. It
does not call the streamed implementation for expected values. Zero-tolerance coverage includes:

- impulse, checkerboard, and ramp inputs;
- odd dimensions, row/tile boundaries, radii 1/2/7/19, radius relationships, and a radius larger than
  both dimensions;
- the legacy radius-zero early-return rule (rolling a nominal radius-zero sum can drift by one ULP);
- global-edge clamping and concurrent-radius delay alignment;
- every pointwise and spatial control, range masks, and stacked adjustments;
- invalid, overflowed, undersized, read-only, and non-zero-position buffers;
- denial before allocation, injected allocation failure, repeated cleanup, and diagnostics; and
- cancellation after real progress at raster/luma/all six filter passes/composition, with exact-once
  cleanup and the documented private-buffer discard contract;
- admission cancellation with byte-unchanged output; and
- radial-feather zero, sub-minimum/minimum, maximum/over-maximum, infinity, and NaN cases across row
  boundaries (non-finite coverage fails closed rather than poisoning RGB).

## Historical release-device evidence (current-source re-run required)

The following earlier run used a minified release target plus a separately minified instrumentation
APK. Both were 16 KiB aligned, debug-signed with the same repository test key, installed, pulled back
from the phone, and hash-compared with the supplied artifacts. It predates the current cancellation
and provenance source, so it is historical performance/memory context, **not** current qualification.

- Device: Samsung SM-S948W, Android API 36.
- Build fingerprint: `samsung/m3qcsx/m3q:16/BP4A.251205.006/S948WVLS4AZG3_OYV4AZG3:user/release-keys`.
- Target SHA-256: `4eb7074d28c865d105b06820458361de65cc6e5a2c1b2c64cd40e7d6921a9323`.
- Test SHA-256: `e68cd3fbddfa9b4ee4220af061901c316bcc28998094c33989a9dba8782a6f1b`.
- Post-run thermal status: `0`; HAL AP 46.2 C, battery 32.9 C, skin 37.2 C; battery 79%, USB powered.

The probe samples process PSS and `/proc/self/statm` RSS every 2 ms. Its output buffer uses Android
API-27 anonymous `SharedMemory`, then explicitly unmaps and closes it. This matches the native/JNI-owned
production buffer contract. `ByteBuffer.allocateDirect(600047616)` was rejected by ART because Android
backs that constructor with a non-movable array under this device's 512 MiB heap limit; using it would
test the wrong ownership model.

| Cell | Baseline / peak / delta PSS | Baseline / peak / delta RSS | Durations | Repeat SHA-256 |
|---|---:|---:|---:|---|
| 12.5 MP, 4 masks x 2 | 8,993 / 216,991 / 207,998 KiB | 73,364 / 286,000 / 212,636 KiB | 15,578.10 / 15,483.95 ms | `ad20e486ea3aedb2b99a5eef82876dbea183a8d1eeafedd7de5578725bb4da21` |
| 50 MP, 4 masks x 2 | 8,728 / 830,235 / 821,507 KiB | 72,204 / 898,180 / 825,976 KiB | 62,063.18 / 61,970.89 ms | `0fdacce5d7b8ac6a65e8ec9bedc3bbd60f6c23b95a2e020a2aed8e77d18930b6` |

Both repeats in each cell produced the same digest. The forced-denial cell also passed before the
memory cells: zero scratch allocations, one denied reservation, zero releases, unchanged image digest
`80dab22a2bbe88fd1076dc35aff3253b9e52736203b9bd5ea5af27a9535ce825`.

For a new run, `tools/ticket141_mask_memory.ps1` writes APK/pulled-artifact evidence under the selected
`build/evidence` directory and durable text under `docs/evidence/ticket141/current/`. The durable set
includes Git commit/tree provenance, hashes of every listed ticket/source/test/build input, exact APK
hashes, source status, denial/memory outputs, and thermal/battery snapshots. It is authoritative only
when `qualification_status.txt` says `status=COMPLETE`; the script re-hashes source inputs and checks
that `HEAD` did not change after both memory cells. See
[`evidence/ticket141/README.md`](evidence/ticket141/README.md).

Neither the historical results nor a future memory qualification closes the 1–2 second performance
objective; that requires a separate accelerated-pipeline benchmark and acceptance gate.

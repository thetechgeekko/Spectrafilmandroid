/*
 * Spektrafilm for Android — GPU (Vulkan compute) fast-path. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * OPT-IN, OFF BY DEFAULT (CMake `SPK_ENABLE_VULKAN`, default OFF). This is the
 * foundation of the GPU offload described in docs/PERF_ROADMAP.md (#1, the real
 * Lightroom-class lever). It runs a per-element engine op on the GPU via a Vulkan
 * compute shader. Under the adopted proxy-approximate / export-exact policy it is a
 * PREVIEW-only acceleration: GPU float math is not bit-identical to the CPU/oracle
 * path, so the export + parity-gated path never call this.
 *
 * Build: when SPK_ENABLE_VULKAN is defined the engine links libvulkan; otherwise this
 * whole module is compiled out and the library is byte-identical to today. The CPU
 * fallback (spk::gpu::available() == false) is always correct.
 */
#ifndef SPK_GPU_VULKAN_COMPUTE_H
#define SPK_GPU_VULKAN_COMPUTE_H

#include <cstddef>
#include <cstdint>

namespace spk::gpu {

// SPDX-FileCopyrightText: 2026 Spektrafilm Android contributors
// SPDX-License-Identifier: GPL-3.0-only
// Resident resource/DAG interface concepts adapted from
// chaert-s/spektrafilm-ofx src/SpektraVulkanRenderer.{h,cpp}, pinned at
// 86476afc5b077de77e2278e3658d1ba9309892a1. Android modifications replace
// OpenFX/desktop types with a bounded pointwise-only request and fail-closed
// diagnostics; no OFX shader math or binary resources cross this interface.

struct PointwiseTableSpan {
    const float* data = nullptr;
    size_t count = 0;
};

struct PointwiseFilmRequest {
    PointwiseTableSpan tc_lut{};          // tc_edge * tc_edge * 3
    uint32_t tc_edge = 0;
    PointwiseTableSpan develop_axis{};    // curve_points * 3
    PointwiseTableSpan develop_curve{};   // curve_points * 3
    PointwiseTableSpan dir_axis{};        // curve_points * 3
    PointwiseTableSpan dir_curve{};       // curve_points * 3
    uint32_t curve_points = 0;
    float exposure_multiplier = 1.0f;
    float coupler_shift = 0.0f;
    float coupler_matrix[9]{};
};

struct PointwisePrintRequest {
    PointwiseTableSpan dye{};                     // 81 * 3, band-major
    PointwiseTableSpan illuminant_sensitivity{};  // 81 * 3, band-major
    PointwiseTableSpan paper_axis{};              // curve_points * 3
    PointwiseTableSpan paper_curve{};             // curve_points * 3
    uint32_t curve_points = 0;
    float midgray = 1.0f;
    float exposure_multiplier = 1.0f;
    float preflash[3]{};
};

struct PointwiseScanRequest {
    PointwiseTableSpan dye{};             // 81 * 3, band-major
    PointwiseTableSpan illuminant_cmf{};  // 81 * 3, band-major
    float xyz_to_rgb[9]{};                // row-major 3x3
};

struct PointwiseDispatchGrid {
    uint32_t groups_x = 0;
    uint32_t groups_y = 0;
    uint32_t total_groups = 0;
    uint64_t group_row_stride_pixels = 0;
    bool valid = false;
};

// Pure, allocation-free planner shared by validation/tests and the Vulkan host.
// It decomposes a frame into one 2D vkCmdDispatch while preserving a flat pixel
// index. Device maxComputeWorkGroupCount[0..1] are passed in explicitly.
PointwiseDispatchGrid plan_pointwise_dispatch(uint32_t pixel_count,
                                              uint32_t max_groups_x,
                                              uint32_t max_groups_y) noexcept;

// One request is one whole-frame pointwise DAG operation. All allocation sizes
// and dispatch dimensions are checked against the selected Vulkan device limits.
struct PointwiseChainRequest {
    const float* input_rgb = nullptr;
    size_t input_component_count = 0;
    uint32_t pixel_count = 0;
    // Opaque nonzero generation owned by the engine's prepared-table cache (not
    // a truncated content digest). It must change whenever any table or shape
    // changes; the GPU cache additionally verifies the complete table layout.
    uint64_t static_table_key = 0;
    PointwiseFilmRequest film{};
    PointwisePrintRequest print{};
    PointwiseScanRequest scan{};
};

struct PointwiseChainOutput {
    float* rgb = nullptr;
    size_t component_capacity = 0;
};

enum class PointwiseFallbackReason : uint8_t {
    none = 0,
    vulkan_disabled,
    unavailable,
    invalid_request,
    request_too_large,
    allocation_failed,
    pipeline_failed,
    upload_failed,
    dispatch_failed,
    readback_failed,
};

struct PointwiseChainDiagnostics {
    bool engaged = false;
    // Frame-operation counters are completion counters: a failed call reports
    // zero even when command recording or submission had already been attempted.
    uint32_t dispatches = 0;
    uint32_t input_uploads = 0;
    uint32_t final_readbacks = 0;
    uint64_t interstage_host_bytes = 0;
    uint32_t pipeline_creates = 0;
    uint32_t buffer_allocations = 0;
    // Host-staged static bytes attempted by this call. Unlike the frame counters
    // above, this may remain nonzero if a later command submission/readback fails.
    uint64_t static_upload_bytes = 0;
    PointwiseFallbackReason fallback_reason = PointwiseFallbackReason::none;
};

// Pointwise filming -> printing -> scan in one Vulkan command buffer. On
// success, exactly three compute dispatches operate on shared device-local
// ping-pong buffers between one input upload and one final readback. Persistent
// pipelines, grow-only buffers, and keyed static f32 tables are reused on warm
// calls. Returns false without modifying the caller's output on any failure.
bool render_pointwise_chain(const PointwiseChainRequest& request,
                            PointwiseChainOutput* output,
                            PointwiseChainDiagnostics* diagnostics) noexcept;

const char* pointwise_fallback_reason_name(PointwiseFallbackReason reason) noexcept;

// True when SPK_ENABLE_VULKAN is compiled in AND a usable Vulkan device + compute
// queue were found at runtime. When false, callers must use the CPU path.
bool available();

// Apply the sRGB CCTF encode + clip to [0,1] to `data` (length `n` interleaved float
// RGB components) on the GPU, in place. Returns false if the GPU path is unavailable
// or any Vulkan call failed (caller then falls back to the CPU path). Never throws.
bool cctf_encode_srgb(float* data, size_t n);

// GPU 81-band spectral SCAN integral (the bottleneck-class kernel, preview-only):
// density_cmy[npix*3] -> output RGB[npix*3], via per-pixel spectral transmittance
// (10^-D over 81 bands) -> XYZ -> output RGB + sRGB CCTF. Spectral tables:
//   dye     : NB*3 per-channel dye densities D_c(lambda)  (band-major c,m,y)
//   icmf    : NB*3 illuminant-premultiplied CMFs          (band-major X,Y,Z)
//   xyz2rgb : 9 floats, row-major 3x3 XYZ->output-RGB matrix (pass Mc.M composed
//             to mirror the engine's full linear chain — see the PR #145 probe)
// Returns false if the GPU path is unavailable or any Vulkan call failed (caller
// falls back to the CPU scan). NOT bit-exact vs the f64 oracle -> preview only;
// the export + parity-gated path never call this (the #149 law revision opens
// oracle-verified GPU export as future M4 work). MEASURED ON DEVICE (PR #145,
// docs/research/gpu-device-probe.md): worst-case max_abs 2.15e-06 / rms 7.07e-08
// vs the f64 chain — 46x/141x inside the oracle tolerance — and byte-identical
// across repeated dispatches (Adreno 840, driver 512.842.19). The host guards
// non-finite densities at upload (NaN/Inf -> 1e4f -> black), so shader NaN
// behaviour never decides pixels.
bool scan_spectral(const float* cmy, float* rgb, uint32_t npix,
                   const float* dye, const float* icmf, const float* xyz2rgb);

// LINEAR variant (GPU M1, #146): the same 81-band integral, but the output is
// UNCLIPPED linear output-space RGB — no CAT02 fold, no CCTF, no clamp — so the
// CPU plane ops (unsharp / lens blur / gamut compression) and the standard
// encode tail run on it unchanged. `xyz2rgb` is the frame's plain XYZ->RGB
// matrix (Mc stays in the CPU encode). Same tables, same fallback contract,
// same preview-only law as scan_spectral.
bool scan_spectral_linear(const float* cmy, float* rgb, uint32_t npix,
                          const float* dye, const float* icmf, const float* xyz2rgb);

}  // namespace spk::gpu

#endif  // SPK_GPU_VULKAN_COMPUTE_H

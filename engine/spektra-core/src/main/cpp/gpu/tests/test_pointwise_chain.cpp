// SPDX-FileCopyrightText: 2026 Spektrafilm Android contributors
// SPDX-License-Identifier: GPL-3.0-only
// Focused connected-device contract gate for the resident pointwise Vulkan DAG.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

#include "gpu/tests/pointwise_chain_oracle.h"
#include "gpu/vulkan_compute.h"

namespace {
int failures = 0;

void check(bool condition, const char* label) {
    std::printf("%s: %s\n", condition ? "ok" : "FAIL", label);
    if (!condition) failures = 1;
}
}  // namespace

int main() {
    using namespace spk::gpu;
    const PointwiseDispatchGrid grid12m =
        plan_pointwise_dispatch(12000000u, 65535u, 65535u);
    const PointwiseDispatchGrid grid50m =
        plan_pointwise_dispatch(50000000u, 65535u, 65535u);
    const PointwiseDispatchGrid grid200m =
        plan_pointwise_dispatch(200000000u, 65535u, 65535u);
    const PointwiseDispatchGrid gridLegacyBoundary =
        plan_pointwise_dispatch(4194241u, 65535u, 65535u);
    check(grid12m.valid && grid12m.groups_y == 3u, "12 MP uses one 2D dispatch");
    check(grid50m.valid && grid50m.groups_y == 12u, "50 MP uses one 2D dispatch");
    check(grid200m.valid && grid200m.groups_y == 48u, "200 MP plan is allocation-free");
    check(gridLegacyBoundary.valid && gridLegacyBoundary.groups_x == 65535u &&
              gridLegacyBoundary.groups_y == 2u,
          "4,194,241 pixels plans a real two-row shader dispatch");

    constexpr uint32_t pixels = 257;
    constexpr uint32_t curvePoints = 5;
    std::vector<float> input(static_cast<size_t>(pixels) * 3u);
    for (uint32_t pixel = 0; pixel < pixels; ++pixel) {
        const float t = static_cast<float>(pixel) / static_cast<float>(pixels - 1u);
        input[pixel * 3u] = 0.03f + 1.17f * t;
        input[pixel * 3u + 1u] = 0.82f - 0.54f * t;
        input[pixel * 3u + 2u] =
            0.11f + 0.43f * static_cast<float>((pixel * 37u) % pixels) /
                        static_cast<float>(pixels - 1u);
    }
    const float tc[12] = {
        0.13f, 0.21f, 0.34f, 0.55f, 0.09f, 0.27f,
        0.18f, 0.63f, 0.41f, 0.77f, 0.46f, 0.12f,
    };
    const float developAxis[curvePoints * 3] = {
        -8.0f, -7.4f, -6.8f, -3.5f, -3.0f, -2.4f, -1.0f, -0.4f, 0.1f,
        0.8f,  1.3f,  1.9f,  4.0f,  4.6f,  5.2f,
    };
    const float developCurve[curvePoints * 3] = {
        0.03f, 0.07f, 0.12f, 0.19f, 0.25f, 0.31f, 0.52f, 0.61f, 0.73f,
        1.08f, 1.21f, 1.37f, 1.92f, 2.13f, 2.36f,
    };
    const float dirAxis[curvePoints * 3] = {
        -7.7f, -7.0f, -6.4f, -3.2f, -2.7f, -2.0f, -0.8f, -0.2f, 0.4f,
        1.0f,  1.6f,  2.2f,  4.3f,  4.9f,  5.6f,
    };
    const float dirCurve[curvePoints * 3] = {
        0.02f, 0.05f, 0.09f, 0.16f, 0.22f, 0.29f, 0.48f, 0.58f, 0.69f,
        1.02f, 1.18f, 1.32f, 1.85f, 2.06f, 2.29f,
    };
    const float paperAxis[curvePoints * 3] = {
        -6.9f, -6.3f, -5.8f, -3.0f, -2.5f, -1.9f, -0.7f, -0.1f, 0.5f,
        1.1f,  1.7f,  2.4f,  4.6f,  5.1f,  5.8f,
    };
    const float paperCurve[curvePoints * 3] = {
        0.04f, 0.08f, 0.13f, 0.21f, 0.27f, 0.34f, 0.56f, 0.66f, 0.78f,
        1.12f, 1.28f, 1.45f, 2.01f, 2.24f, 2.49f,
    };
    std::vector<float> dye(81u * 3u);
    std::vector<float> scanDye(81u * 3u);
    std::vector<float> printResponse(81u * 3u);
    std::vector<float> scanResponse(81u * 3u);
    for (size_t band = 0; band < 81; ++band) {
        const float t = static_cast<float>(band + 1u) / 81.0f;
        dye[band * 3u] = 0.002f + 0.006f * t;
        dye[band * 3u + 1u] = 0.003f + 0.004f * t * t;
        dye[band * 3u + 2u] = 0.0015f + 0.007f * (1.0f - t);
        scanDye[band * 3u] = 0.004f + 0.003f * (1.0f - t);
        scanDye[band * 3u + 1u] = 0.001f + 0.008f * t * t;
        scanDye[band * 3u + 2u] = 0.0025f + 0.005f * t;
        printResponse[band * 3u] = (0.60f + 0.40f * t) / 81.0f;
        printResponse[band * 3u + 1u] = (0.72f - 0.31f * t) / 81.0f;
        printResponse[band * 3u + 2u] = (0.48f + 0.23f * t * t) / 81.0f;
        scanResponse[band * 3u] = (0.51f + 0.36f * t) / 81.0f;
        scanResponse[band * 3u + 1u] = (0.79f - 0.27f * t) / 81.0f;
        scanResponse[band * 3u + 2u] = (0.43f + 0.29f * t * t) / 81.0f;
    }

    PointwiseChainRequest request{};
    request.input_rgb = input.data();
    request.input_component_count = input.size();
    request.pixel_count = pixels;
    request.static_table_key = UINT64_C(0x1480000000000002);
    request.film.tc_lut = {tc, 12};
    request.film.tc_edge = 2;
    request.film.develop_axis = {developAxis, curvePoints * 3u};
    request.film.develop_curve = {developCurve, curvePoints * 3u};
    request.film.dir_axis = {dirAxis, curvePoints * 3u};
    request.film.dir_curve = {dirCurve, curvePoints * 3u};
    request.film.curve_points = curvePoints;
    request.film.exposure_multiplier = 0.83f;
    request.film.coupler_shift = 0.17f;
    const float couplerMatrix[9] = {
        0.041f, 0.012f, 0.007f, 0.009f, 0.053f,
        0.015f, 0.004f, 0.018f, 0.047f,
    };
    std::memcpy(request.film.coupler_matrix, couplerMatrix, sizeof(couplerMatrix));
    request.print.dye = {dye.data(), dye.size()};
    request.print.illuminant_sensitivity = {printResponse.data(), printResponse.size()};
    request.print.paper_axis = {paperAxis, curvePoints * 3u};
    request.print.paper_curve = {paperCurve, curvePoints * 3u};
    request.print.curve_points = curvePoints;
    request.print.midgray = 0.91f;
    request.print.exposure_multiplier = 1.13f;
    request.print.preflash[0] = 0.011f;
    request.print.preflash[1] = 0.023f;
    request.print.preflash[2] = 0.006f;
    request.scan.dye = {scanDye.data(), scanDye.size()};
    request.scan.illuminant_cmf = {scanResponse.data(), scanResponse.size()};
    const float xyzToRgb[9] = {
        1.07f, 0.08f, 0.02f, 0.04f, 0.92f,
        0.07f, 0.03f, 0.11f, 0.79f,
    };
    std::memcpy(request.scan.xyz_to_rgb, xyzToRgb, sizeof(xyzToRgb));

    std::vector<float> untouched(input.size(), 123.0f);
    PointwiseChainOutput invalidOutput{untouched.data(), untouched.size()};
    PointwiseChainRequest invalid = request;
    --invalid.film.develop_axis.count;
    PointwiseChainDiagnostics invalidDiagnostics{};
    check(!render_pointwise_chain(invalid, &invalidOutput, &invalidDiagnostics),
          "truncated static table fails closed");
    bool sentinel = true;
    for (float v : untouched) sentinel = sentinel && v == 123.0f;
    check(sentinel, "invalid request never modifies output");

    const bool gpu = available();
    std::printf("info: vulkan_available=%d\n", gpu ? 1 : 0);
    std::vector<float> cold(input.size(), -1.0f);
    PointwiseChainOutput coldOutput{cold.data(), cold.size()};
    PointwiseChainDiagnostics coldDiagnostics{};
    const bool coldOk = render_pointwise_chain(request, &coldOutput, &coldDiagnostics);
    if (!gpu) {
        check(!coldOk && !coldDiagnostics.engaged,
              "unavailable Vulkan preserves fail-closed behavior");
        return failures;
    }

    std::printf("info: cold dispatch=%u upload=%u readback=%u pipeline=%u alloc=%u static=%llu\n",
                coldDiagnostics.dispatches, coldDiagnostics.input_uploads,
                coldDiagnostics.final_readbacks, coldDiagnostics.pipeline_creates,
                coldDiagnostics.buffer_allocations,
                static_cast<unsigned long long>(coldDiagnostics.static_upload_bytes));
    check(coldOk && coldDiagnostics.engaged, "cold resident chain engages");
    check(coldDiagnostics.dispatches == 3 && coldDiagnostics.input_uploads == 1 &&
              coldDiagnostics.final_readbacks == 1 &&
              coldDiagnostics.interstage_host_bytes == 0,
          "cold chain is exactly upload -> 3 dispatches -> readback");
    check(coldDiagnostics.pipeline_creates == 3 && coldDiagnostics.buffer_allocations > 0 &&
              coldDiagnostics.static_upload_bytes > 0,
          "cold chain creates and uploads persistent resources");
    bool finite = true;
    for (float v : cold) finite = finite && std::isfinite(v);
    check(finite, "explicit shader containment produces finite output");
    std::vector<double> oracle(input.size());
    spk::gpu::test::render_pointwise_chain_f64(request, oracle.data());
    const spk::gpu::test::PointwiseOracleError oracleError =
        spk::gpu::test::pointwise_oracle_error(cold.data(), oracle.data(), cold.size());
    std::printf("info: f64 oracle max_abs=%.9g rms=%.9g\n", oracleError.max_abs,
                oracleError.rms);
    check(oracleError.max_abs <= 2e-5 && oracleError.rms <= 5e-6,
          "combined resident chain stays inside the f64 oracle tolerance");

    std::vector<float> nanInput = input;
    nanInput[3] = std::numeric_limits<float>::quiet_NaN();
    PointwiseChainRequest nanRequest = request;
    nanRequest.input_rgb = nanInput.data();
    std::vector<float> nanResult(input.size(), -4.0f);
    PointwiseChainOutput nanOutput{nanResult.data(), nanResult.size()};
    PointwiseChainDiagnostics nanDiagnostics{};
    const bool nanOk = render_pointwise_chain(nanRequest, &nanOutput, &nanDiagnostics);
    bool nanFinite = true;
    for (float value : nanResult) nanFinite = nanFinite && std::isfinite(value);
    check(nanOk && nanDiagnostics.engaged && nanDiagnostics.dispatches == 3 &&
              nanDiagnostics.input_uploads == 1 &&
              nanDiagnostics.final_readbacks == 1 &&
              nanDiagnostics.interstage_host_bytes == 0 && nanFinite,
          "separate NaN render is explicitly contained");

    std::vector<float> warm(input.size(), -2.0f);
    PointwiseChainOutput warmOutput{warm.data(), warm.size()};
    PointwiseChainDiagnostics warmDiagnostics{};
    const bool warmOk = render_pointwise_chain(request, &warmOutput, &warmDiagnostics);
    std::printf("info: warm dispatch=%u upload=%u readback=%u pipeline=%u alloc=%u static=%llu\n",
                warmDiagnostics.dispatches, warmDiagnostics.input_uploads,
                warmDiagnostics.final_readbacks, warmDiagnostics.pipeline_creates,
                warmDiagnostics.buffer_allocations,
                static_cast<unsigned long long>(warmDiagnostics.static_upload_bytes));
    check(warmOk && warmDiagnostics.engaged, "warm resident chain engages");
    check(std::memcmp(cold.data(), warm.data(), cold.size() * sizeof(float)) == 0,
          "warm resident chain is byte-deterministic");
    check(warmDiagnostics.dispatches == 3 && warmDiagnostics.input_uploads == 1 &&
              warmDiagnostics.final_readbacks == 1 &&
              warmDiagnostics.interstage_host_bytes == 0,
          "warm chain preserves exact transfer/dispatch counters");
    check(warmDiagnostics.pipeline_creates == 0 && warmDiagnostics.buffer_allocations == 0 &&
              warmDiagnostics.static_upload_bytes == 0,
          "warm chain reuses pipelines, buffers, and static tables");

    bool repeatsOk = true;
    for (uint32_t repeat = 0; repeat < 100; ++repeat) {
        std::vector<float> repeated(input.size(), -3.0f);
        PointwiseChainOutput repeatedOutput{repeated.data(), repeated.size()};
        PointwiseChainDiagnostics repeatedDiagnostics{};
        repeatsOk = repeatsOk &&
                    render_pointwise_chain(request, &repeatedOutput, &repeatedDiagnostics) &&
                    repeatedDiagnostics.engaged && repeatedDiagnostics.dispatches == 3 &&
                    repeatedDiagnostics.input_uploads == 1 &&
                    repeatedDiagnostics.final_readbacks == 1 &&
                    repeatedDiagnostics.interstage_host_bytes == 0 &&
                    repeatedDiagnostics.pipeline_creates == 0 &&
                    repeatedDiagnostics.buffer_allocations == 0 &&
                    repeatedDiagnostics.static_upload_bytes == 0 &&
                    std::memcmp(cold.data(), repeated.data(), cold.size() * sizeof(float)) == 0;
    }
    check(repeatsOk,
          "100 warm dispatches are byte-deterministic with zero resource churn");

    float changedTc[12];
    for (size_t i = 0; i < 12; ++i) {
        changedTc[i] = tc[i] * (0.91f + 0.01f * static_cast<float>(i)) + 0.017f;
    }
    PointwiseChainRequest changedRequest = request;
    changedRequest.static_table_key = UINT64_C(0x1480000000001002);
    changedRequest.film.tc_lut = {changedTc, 12};
    std::vector<float> changed(input.size(), -6.0f);
    PointwiseChainOutput changedOutput{changed.data(), changed.size()};
    PointwiseChainDiagnostics changedDiagnostics{};
    const bool changedOk =
        render_pointwise_chain(changedRequest, &changedOutput, &changedDiagnostics);
    check(changedOk && changedDiagnostics.engaged && changedDiagnostics.dispatches == 3 &&
              changedDiagnostics.input_uploads == 1 &&
              changedDiagnostics.final_readbacks == 1 &&
              changedDiagnostics.interstage_host_bytes == 0,
          "changed table key completes the resident DAG");
    check(changedDiagnostics.pipeline_creates == 0 &&
              changedDiagnostics.buffer_allocations == 0 &&
              changedDiagnostics.static_upload_bytes > 0,
          "changed table key re-uploads static bytes without resource rebuild");
    check(std::memcmp(cold.data(), changed.data(), cold.size() * sizeof(float)) != 0,
          "changed static table changes output");
    std::vector<double> changedOracle(input.size());
    spk::gpu::test::render_pointwise_chain_f64(changedRequest, changedOracle.data());
    const spk::gpu::test::PointwiseOracleError changedError =
        spk::gpu::test::pointwise_oracle_error(changed.data(), changedOracle.data(),
                                               changed.size());
    std::printf("info: changed-table f64 oracle max_abs=%.9g rms=%.9g\n",
                changedError.max_abs, changedError.rms);
    check(changedError.max_abs <= 2e-5 && changedError.rms <= 5e-6,
          "changed-table chain stays inside the f64 oracle tolerance");

    try {
        constexpr uint32_t boundaryPixels = 4194241u;
        const size_t boundaryComponents = static_cast<size_t>(boundaryPixels) * 3u;
        std::vector<float> boundaryInput(boundaryComponents, 0.18f);
        std::vector<float> boundaryResult(boundaryComponents, -5.0f);
        PointwiseChainRequest boundaryRequest = request;
        boundaryRequest.input_rgb = boundaryInput.data();
        boundaryRequest.input_component_count = boundaryInput.size();
        boundaryRequest.pixel_count = boundaryPixels;
        PointwiseChainOutput boundaryOutput{boundaryResult.data(), boundaryResult.size()};
        PointwiseChainDiagnostics boundaryDiagnostics{};
        const bool boundaryOk =
            render_pointwise_chain(boundaryRequest, &boundaryOutput, &boundaryDiagnostics);
        std::printf(
            "info: boundary_4194241 ok=%d reason=%s dispatch=%u upload=%u "
            "readback=%u interstage=%llu pipeline=%u alloc=%u static=%llu\n",
            boundaryOk ? 1 : 0,
            pointwise_fallback_reason_name(boundaryDiagnostics.fallback_reason),
            boundaryDiagnostics.dispatches, boundaryDiagnostics.input_uploads,
            boundaryDiagnostics.final_readbacks,
            static_cast<unsigned long long>(boundaryDiagnostics.interstage_host_bytes),
            boundaryDiagnostics.pipeline_creates, boundaryDiagnostics.buffer_allocations,
            static_cast<unsigned long long>(boundaryDiagnostics.static_upload_bytes));
        if (boundaryOk) {
            check(boundaryDiagnostics.engaged && boundaryDiagnostics.dispatches == 3 &&
                      boundaryDiagnostics.input_uploads == 1 &&
                      boundaryDiagnostics.final_readbacks == 1 &&
                      boundaryDiagnostics.interstage_host_bytes == 0,
                  "4,194,241 pixels executes one upload, three 2D dispatches, one readback");
            bool boundaryFinite = true;
            for (float value : boundaryResult) {
                boundaryFinite = boundaryFinite && std::isfinite(value);
            }
            check(boundaryFinite && boundaryResult.front() != -5.0f &&
                      boundaryResult.back() != -5.0f,
                  "two-row shader dispatch writes finite first and final pixels");
        } else {
            bool boundaryUntouched = true;
            for (float value : boundaryResult) {
                boundaryUntouched = boundaryUntouched && value == -5.0f;
            }
            const bool cleanFallback =
                !boundaryDiagnostics.engaged && boundaryDiagnostics.dispatches == 0 &&
                boundaryDiagnostics.input_uploads == 0 &&
                boundaryDiagnostics.final_readbacks == 0 &&
                boundaryDiagnostics.interstage_host_bytes == 0 && boundaryUntouched &&
                (boundaryDiagnostics.fallback_reason ==
                     PointwiseFallbackReason::request_too_large ||
                 boundaryDiagnostics.fallback_reason ==
                     PointwiseFallbackReason::allocation_failed);
            check(cleanFallback,
                  "4,194,241-pixel device-limit fallback is explicit and output-safe");
        }
    } catch (const std::bad_alloc&) {
        check(false, "host allocates the 4,194,241-pixel boundary fixture");
    }

    std::printf("test_pointwise_chain: %s\n", failures ? "FAIL" : "ALL OK");
    return failures;
}

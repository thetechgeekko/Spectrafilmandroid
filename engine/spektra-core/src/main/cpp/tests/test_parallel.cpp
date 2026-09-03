/*
 * Spektrafilm for Android — thread-count invariance (determinism) test.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * The per-pixel engine stages (expose / scan / print_expose) are parallelized
 * via kernels/parallel (deterministic fork-join over disjoint pixel ranges).
 * Because each output pixel is an independent function of its input pixel, the
 * result MUST be byte-identical regardless of the worker count. This test runs
 * the SAME input through spk_simulate at SPK_NUM_THREADS=1 and =8 and asserts the
 * outputs are bitwise equal (memcmp) — for the scan route, the print route, with
 * grain + halation ON (the stochastic/spatial branch), and on a synthesized
 * multi-block image large enough that the grain stage's fixed-block dynamic
 * scheduler really runs blocks on concurrent workers.
 *
 * Build (host) — full source set, run from the cpp root:
 *   g++ -std=c++17 -O2 -pthread -I <cpp_root> -I <tools/parity> \
 *     tests/test_parallel.cpp spektra.cpp \
 *     model/*.cpp kernels/*.cpp io/*.cpp profiles/*.cpp \
 *     runtime/params.cpp runtime/print_digest.cpp runtime/stages/*.cpp \
 *     -o /tmp/test_parallel
 * Run:
 *   /tmp/test_parallel <asset_dir> <input.f64>
 */
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "runtime/stage_timer.h"
#include "spektra.h"

namespace {

const char* kAssetDir = "/home/user/spektrafilm/src/spektrafilm/data";
const char* kInputF64 =
    "/home/user/Spectrafilmandroid/engine/spektra-core/src/main/cpp/tests/"
    "scan_portra_input_rgb.f64";

// Run spk_simulate with a forced worker count on a FRESH engine; copy the
// output into `out`. A fresh engine per run is essential: the engine-level
// film-density memo would otherwise let the second thread-count run HIT the
// first run's slot, making the 1-vs-8 comparison of the filming stage vacuous.
// Returns false on engine error.
bool simulate_with_threads(const std::string& asset_dir, const spk_image* in,
                           const spk_params* p, int nthreads,
                           std::vector<float>* out) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", nthreads);
    setenv("SPK_NUM_THREADS", buf, /*overwrite=*/1);

    spk_engine* eng = nullptr;
    spk_status st = spk_engine_create(asset_dir.c_str(), &eng);
    if (st != SPK_OK) {
        std::fprintf(stderr, "engine create (threads=%d) failed: %s\n", nthreads,
                     spk_status_str(st));
        return false;
    }
    spk_image img{};
    st = spk_simulate(eng, in, p, &img);
    if (st != SPK_OK) {
        std::fprintf(stderr, "spk_simulate (threads=%d) failed: %s\n", nthreads,
                     spk_status_str(st));
        spk_engine_destroy(eng);
        return false;
    }
    const size_t n = static_cast<size_t>(img.width) * img.height * 3;
    out->assign(img.data, img.data + n);
    spk_image_free(&img);
    spk_engine_destroy(eng);
    return true;
}

// Assert two run outputs are bitwise identical; print + return PASS/FAIL.
bool check_identical(const char* label, const std::vector<float>& a,
                     const std::vector<float>& b,
                     const char* what = "1-thread vs 8-thread") {
    if (a.size() != b.size()) {
        std::printf("[%s] size mismatch %zu vs %zu -> FAIL\n", label, a.size(),
                    b.size());
        return false;
    }
    const bool same = std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
    // Also report the worst absolute difference for diagnostics (0 when identical).
    double max_abs = 0.0;
    size_t argmax = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        if (d < 0) d = -d;
        if (d > max_abs) { max_abs = d; argmax = i; }
    }
    std::printf("[%s] %s: max_abs=%.3e (worst idx=%zu) -> %s\n",
                label, what, max_abs, argmax, same ? "PASS" : "FAIL");
    return same;
}

// Two native renders can overlap on different Dispatchers.Default threads. Make
// that overlap deterministic without running the expensive image pipeline: A
// opens nested preview spans, B records/completes an export, then A completes.
// This gates caller isolation, publish-after-completion, unique correlation ids,
// outcome metadata, stable JSON, and parent/child/wall reconciliation (#163).
bool check_stage_timing_isolation() {
    std::promise<void> a_started_promise;
    std::promise<void> b_done_promise;
    std::shared_future<void> a_started = a_started_promise.get_future().share();
    std::shared_future<void> b_done = b_done_promise.get_future().share();
    std::string a_timings;
    std::string b_timings;
    std::string a_json;
    std::string b_json;
    spk::StageTimingSnapshot a_snapshot;
    spk::StageTimingSnapshot b_snapshot;

    std::thread a([&] {
        {
            spk::ScopedRenderTiming render(spk::RTK_PREVIEW);
            {
                spk::ScopedStage preprocess(spk::STG_PREPROCESS);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            {
                spk::ScopedStage scan(spk::STG_SCAN);
                {
                    spk::ScopedStage spatial(spk::STG_SCAN_SPATIAL);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                a_started_promise.set_value();
                b_done.wait();
            }
            {
                spk::ScopedStageTimingSuppression suppress;
                {
                    spk::ScopedStage hidden_develop(spk::STG_DEVELOP);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                {
                    spk::ScopedStageTimingSuppression nested_suppress;
                    spk::ScopedStage hidden_print(spk::STG_PRINT_EXPOSE);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            spk::stage_timing_note_gpu_pointwise(
                /*requested=*/true, /*attempted=*/true, /*engaged=*/true,
                "none", /*dispatches=*/3, /*input_uploads=*/1,
                /*final_readbacks=*/1, /*interstage_host_bytes=*/0,
                /*pipeline_creates=*/3, /*buffer_allocations=*/4,
                /*static_upload_bytes=*/1234);
            spk::stage_timing_note_gpu_pointwise_self_test(
                "ran_pass", /*duration_ms=*/4.25, /*chain_runs=*/2,
                /*dispatches=*/6, /*input_uploads=*/2,
                /*final_readbacks=*/2, /*interstage_host_bytes=*/0,
                /*pipeline_creates=*/3, /*buffer_allocations=*/4,
                /*static_upload_bytes=*/5678);
            render.finish(0);
        }
        char buf[256];
        spk_stage_timings(buf, sizeof(buf));
        a_timings = buf;
        char json[2048];
        spk_stage_timings_json(json, sizeof(json));
        a_json = json;
        a_snapshot = spk::stage_timing_snapshot();
    });
    std::thread b([&] {
        a_started.wait();
        {
            spk::ScopedRenderTiming render(spk::RTK_EXPORT);
            {
                spk::ScopedStage grain(spk::STG_GRAIN);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            render.finish(0);
        }
        char buf[256];
        spk_stage_timings(buf, sizeof(buf));
        b_timings = buf;
        char json[2048];
        spk_stage_timings_json(json, sizeof(json));
        b_json = json;
        b_snapshot = spk::stage_timing_snapshot();
        b_done_promise.set_value();
    });
    a.join();
    b.join();

    const bool isolated =
        a_timings.find("preprocess=") != std::string::npos &&
        a_timings.find("scan=") != std::string::npos &&
        a_timings.find("scan_spatial=") != std::string::npos &&
        a_timings.find("grain=") == std::string::npos &&
        b_timings.find("grain=") != std::string::npos &&
        b_timings.find("preprocess=") == std::string::npos &&
        b_timings.find("scan=") == std::string::npos;
    const bool ids_ok =
        a_snapshot.render_id != 0 && b_snapshot.render_id != 0 &&
        a_snapshot.render_id != b_snapshot.render_id;
    const bool metadata_ok =
        a_snapshot.kind == spk::RTK_PREVIEW &&
        b_snapshot.kind == spk::RTK_EXPORT &&
        a_snapshot.outcome == spk::RTO_OK &&
        b_snapshot.outcome == spk::RTO_OK &&
        a_snapshot.status_code == 0 && b_snapshot.status_code == 0;
    const bool pointwise_isolation_ok =
        a_snapshot.gpu_pointwise.requested &&
        a_snapshot.gpu_pointwise.attempted &&
        a_snapshot.gpu_pointwise.engaged &&
        std::strcmp(a_snapshot.gpu_pointwise.reason, "none") == 0 &&
        a_snapshot.gpu_pointwise.dispatches == 3 &&
        a_snapshot.gpu_pointwise.input_uploads == 1 &&
        a_snapshot.gpu_pointwise.final_readbacks == 1 &&
        a_snapshot.gpu_pointwise.interstage_host_bytes == 0 &&
        a_snapshot.gpu_pointwise.pipeline_creates == 3 &&
        a_snapshot.gpu_pointwise.buffer_allocations == 4 &&
        a_snapshot.gpu_pointwise.static_upload_bytes == 1234 &&
        std::strcmp(a_snapshot.gpu_pointwise.self_test_state,
                    "ran_pass") == 0 &&
        a_snapshot.gpu_pointwise.self_test_duration_ms == 4.25 &&
        a_snapshot.gpu_pointwise.self_test_chain_runs == 2 &&
        a_snapshot.gpu_pointwise.self_test_dispatches == 6 &&
        a_snapshot.gpu_pointwise.self_test_input_uploads == 2 &&
        a_snapshot.gpu_pointwise.self_test_final_readbacks == 2 &&
        a_snapshot.gpu_pointwise.self_test_interstage_host_bytes == 0 &&
        a_snapshot.gpu_pointwise.self_test_pipeline_creates == 3 &&
        a_snapshot.gpu_pointwise.self_test_buffer_allocations == 4 &&
        a_snapshot.gpu_pointwise.self_test_static_upload_bytes == 5678 &&
        !b_snapshot.gpu_pointwise.requested &&
        !b_snapshot.gpu_pointwise.attempted &&
        !b_snapshot.gpu_pointwise.engaged &&
        std::strcmp(b_snapshot.gpu_pointwise.reason, "not_requested") == 0 &&
        std::strcmp(b_snapshot.gpu_pointwise.self_test_state,
                    "not_run") == 0 &&
        b_snapshot.gpu_pointwise.self_test_duration_ms == 0.0 &&
        b_snapshot.gpu_pointwise.self_test_chain_runs == 0 &&
        b_snapshot.gpu_pointwise.self_test_dispatches == 0;
    const bool reconciliation_ok =
        a_snapshot.stages_ms[spk::STG_DEVELOP] == 0.0 &&
        a_snapshot.stages_ms[spk::STG_PRINT_EXPOSE] == 0.0 &&
        a_snapshot.stages_ms[spk::STG_SCAN] >=
            a_snapshot.stages_ms[spk::STG_SCAN_SPATIAL] &&
        a_snapshot.wall_ms + 0.001 >=
            spk::stage_timing_top_level_ms(a_snapshot) &&
        b_snapshot.wall_ms + 0.001 >=
            spk::stage_timing_top_level_ms(b_snapshot);
    const std::string a_id =
        "\"render_id\":" + std::to_string(a_snapshot.render_id);
    const std::string b_id =
        "\"render_id\":" + std::to_string(b_snapshot.render_id);
    const bool json_ok =
        a_json.find("\"schema\":\"spk.stage_timings.v1\"") !=
            std::string::npos &&
        a_json.find(a_id) != std::string::npos &&
        a_json.find("\"kind\":\"preview\"") != std::string::npos &&
        a_json.find("\"native_outcome\":\"ok\"") != std::string::npos &&
        a_json.find(
            "\"gpu_pointwise\":{\"requested\":true,\"attempted\":true,"
            "\"engaged\":true,\"reason\":\"none\",\"dispatches\":3,"
            "\"input_uploads\":1,\"final_readbacks\":1,"
             "\"interstage_host_bytes\":0,\"pipeline_creates\":3,"
             "\"buffer_allocations\":4,\"static_upload_bytes\":1234,"
             "\"self_test_state\":\"ran_pass\","
             "\"self_test_duration_ms\":4.250,"
             "\"self_test_chain_runs\":2,\"self_test_dispatches\":6,"
             "\"self_test_input_uploads\":2,"
             "\"self_test_final_readbacks\":2,"
             "\"self_test_interstage_host_bytes\":0,"
             "\"self_test_pipeline_creates\":3,"
             "\"self_test_buffer_allocations\":4,"
             "\"self_test_static_upload_bytes\":5678}") !=
             std::string::npos &&
        a_json.find("\"scan_spatial\":\"scan\"") != std::string::npos &&
        b_json.find(b_id) != std::string::npos &&
        b_json.find("\"kind\":\"export\"") != std::string::npos &&
        b_json.find(
            "\"gpu_pointwise\":{\"requested\":false,\"attempted\":false,"
            "\"engaged\":false,\"reason\":\"not_requested\","
            "\"dispatches\":0,\"input_uploads\":0,"
             "\"final_readbacks\":0,\"interstage_host_bytes\":0,"
             "\"pipeline_creates\":0,\"buffer_allocations\":0,"
             "\"static_upload_bytes\":0,"
             "\"self_test_state\":\"not_run\","
             "\"self_test_duration_ms\":0.000,"
             "\"self_test_chain_runs\":0,\"self_test_dispatches\":0,"
             "\"self_test_input_uploads\":0,"
             "\"self_test_final_readbacks\":0,"
             "\"self_test_interstage_host_bytes\":0,"
             "\"self_test_pipeline_creates\":0,"
             "\"self_test_buffer_allocations\":0,"
             "\"self_test_static_upload_bytes\":0}") != std::string::npos;

    // Expected failures and actual exception unwinding must publish error
    // snapshots instead of leaving the previous successful render current.
    {
        spk::ScopedRenderTiming error(spk::RTK_TAP);
        error.finish(SPK_ERR_INTERNAL);
    }
    char error_json[2048];
    spk_stage_timings_json(error_json, sizeof(error_json));
    const bool error_ok =
        std::strstr(error_json, "\"kind\":\"tap\"") != nullptr &&
        std::strstr(error_json, "\"native_outcome\":\"error\"") != nullptr &&
        std::strstr(error_json, "\"status_code\":5") != nullptr;

    try {
        spk::ScopedRenderTiming unwinding(spk::RTK_EXACT_RENDER);
        throw std::runtime_error("timing unwind probe");
    } catch (const std::runtime_error&) {
    }
    const spk::StageTimingSnapshot unwind_snapshot =
        spk::stage_timing_snapshot();
    const bool unwind_ok =
        unwind_snapshot.outcome == spk::RTO_ERROR &&
        unwind_snapshot.status_code == -1 &&
        unwind_snapshot.kind == spk::RTK_EXACT_RENDER;

    char small_json[64] = {'x', '\0'};
    const bool truncation_ok =
        spk_stage_timings_json(small_json, sizeof(small_json)) == 0 &&
        small_json[0] == '\0';
    {
        spk::ScopedRenderTiming escaped(spk::RTK_EXPORT);
        spk::stage_timing_note_gpu_pointwise(
            true, true, false, "dispatch\"failed\\probe\n", 0, 0, 0, 0, 0,
            0, 0);
        escaped.finish(0);
    }
    char escaped_json[2048];
    spk_stage_timings_json(escaped_json, sizeof(escaped_json));
    const bool escaping_ok =
        std::strstr(escaped_json,
                    "\"reason\":\"dispatch\\\"failed\\\\probe\\n\"") !=
        nullptr;
    char outcome_json[256];
    const bool app_outcome_ok =
        spk::stage_timing_outcome_json_format(
            outcome_json, sizeof(outcome_json), a_snapshot.render_id,
            spk::ARO_SUPERSEDED) > 0 &&
        std::strstr(outcome_json, "\"schema\":\"spk.render_outcome.v1\"") !=
            nullptr &&
        std::strstr(outcome_json, "\"app_outcome\":\"superseded\"") !=
            nullptr;

    const bool ok = isolated && ids_ok && metadata_ok && pointwise_isolation_ok &&
                    reconciliation_ok && json_ok && error_ok && unwind_ok &&
                    truncation_ok && escaping_ok && app_outcome_ok;
    std::printf("[stage timing overlap] A={%s} B={%s} ids=%llu/%llu -> %s\n",
                a_timings.c_str(), b_timings.c_str(),
                static_cast<unsigned long long>(a_snapshot.render_id),
                static_cast<unsigned long long>(b_snapshot.render_id),
                ok ? "PASS" : "FAIL");
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    std::string asset_dir  = argc > 1 ? argv[1] : kAssetDir;
    std::string input_path = argc > 2 ? argv[2] : kInputF64;

    // Force genuine multi-chunk execution on this fixture.
    //
    // The fixture is 64x64 = 4096 pixels, which is BELOW kParallelMinChunk (8192).
    // parallel_for therefore clamps to a single chunk and takes the serial path at
    // EVERY thread count, so without this override the thread-invariance assertions
    // below compare serial output against serial output and cannot fail — the gate
    // passes vacuously for the very loops it exists to protect. Lowering the minimum
    // chunk makes 1-vs-N a real comparison of split work against serial work.
    // Production behaviour is untouched: the override is only set here, in the test.
    ::setenv("SPK_PARALLEL_MIN_CHUNK", "256", /*overwrite=*/1);

    // The fixture is a 64x64x3 float64 image (matches make_test_image(64)).
    const int width = 64, height = 64;
    const int npix = width * height;
    std::vector<double> rgb64(static_cast<size_t>(npix) * 3);
    {
        std::ifstream in(input_path, std::ios::binary);
        if (!in) { std::fprintf(stderr, "cannot open %s\n", input_path.c_str()); return 2; }
        in.read(reinterpret_cast<char*>(rgb64.data()),
                static_cast<std::streamsize>(rgb64.size() * sizeof(double)));
        if (in.gcount() != static_cast<std::streamsize>(rgb64.size() * sizeof(double))) {
            std::fprintf(stderr, "input size mismatch\n");
            return 2;
        }
    }
    std::vector<float> rgb32(rgb64.begin(), rgb64.end());
    spk_image in_img{rgb32.data(), width, height, /*color_space=*/SPK_CS_PROPHOTO};

    // Base parity-style params (deterministic). Each case toggles from here.
    spk_params base{};
    base.film_profile = "kodak_portra_400";
    base.print_profile = "kodak_portra_endura";
    spk_default_params(&base);
    base.exposure_compensation_ev = 0.0f;
    base.auto_exposure = 0;
    base.density_curve_gamma = 1.0f;
    base.grain_active = 0;
    base.halation_active = 0;
    base.dir_couplers_active = 1;
    base.glare_active = 0;
    base.output_color_space = SPK_CS_SRGB;
    base.output_cctf_encoding = 1;
    base.rgb_to_raw_method = SPK_RGB2RAW_HANATOS2025;
    base.preview_max_size = 640;

    bool ok = true;
    std::vector<float> r1, r8;

    // 0) Render-local diagnostics: overlapping callers must never reset or
    // merge each other's stage timings (#163).
    ok &= check_stage_timing_isolation();

    // 1) Scan route, pointwise (the threaded expose + scan hot loops).
    {
        spk_params p = base;
        p.scan_film = 1;
        ok &= simulate_with_threads(asset_dir, &in_img, &p, 1, &r1);
        ok &= simulate_with_threads(asset_dir, &in_img, &p, 8, &r8);
        ok &= check_identical("scan_film", r1, r8);
    }

    // 2) Print route (adds the threaded print_expose hot loop).
    {
        spk_params p = base;
        p.scan_film = 0;
        ok &= simulate_with_threads(asset_dir, &in_img, &p, 1, &r1);
        ok &= simulate_with_threads(asset_dir, &in_img, &p, 8, &r8);
        ok &= check_identical("print", r1, r8);
    }

    // 3) Scan route with grain + halation ON: the stochastic + spatial branch.
    //    Grain walks a seeded RNG in pixel order (fixed-block scheduling), and
    //    the spatial blurs are chunked over rows/columns with per-row/per-column
    //    state — all of it must be byte-identical across worker counts.
    {
        spk_params p = base;
        p.scan_film = 1;
        p.grain_active = 1;
        p.halation_active = 1;
        ok &= simulate_with_threads(asset_dir, &in_img, &p, 1, &r1);
        ok &= simulate_with_threads(asset_dir, &in_img, &p, 8, &r8);
        ok &= check_identical("scan_film+grain+halation", r1, r8);
    }

    // 4) PRINT route with grain + halation ON: the print-route spatial + grain
    //    filming branch (the negative's halation/scatter/DIR diffusion + AgX
    //    grain now feed the enlarger). Same thread-invariance contract.
    {
        spk_params p = base;
        p.scan_film = 0;
        p.grain_active = 1;
        p.halation_active = 1;
        ok &= simulate_with_threads(asset_dir, &in_img, &p, 1, &r1);
        ok &= simulate_with_threads(asset_dir, &in_img, &p, 8, &r8);
        ok &= check_identical("print+grain+halation", r1, r8);
    }

    // 5) MULTI-BLOCK grain. The 64x64 fixture is 4096 px < kGrainBlockPixels
    //    (8192, model/grain.cpp), so scenarios 3-4 run the whole grain field as
    //    ONE block on a lone worker — they prove single-block reproducibility,
    //    not that the grain stage's dynamic block scheduler is thread-invariant.
    //    Synthesize a 192x160 image: 30,720 px, strictly > 2*8192, giving FOUR
    //    fixed 8192-px blocks, so at SPK_NUM_THREADS=8 several workers really do
    //    pull blocks concurrently from the atomic counter (worker count clamps
    //    to nblocks=4). Each block must produce the same bytes whichever worker
    //    runs it and in whatever order — assert 1 vs 8 workers memcmp-identical
    //    (max_abs==0). Input is deterministic: the 64x64 fixture tiled with
    //    wraparound, scaled by a horizontal ramp so blocks see different density
    //    statistics (uneven per-block cost is exactly what the dynamic scheduler
    //    exists for). Grain only — halation's (now-parallel) blurs are already
    //    covered by scenarios 3-4 and would only slow the gate down.
    {
        const int w2 = 192, h2 = 160;
        const int npix2 = w2 * h2;  // 30,720 px -> 4 grain blocks of <=8192
        std::vector<float> rgb2(static_cast<size_t>(npix2) * 3);
        for (int y = 0; y < h2; ++y) {
            for (int x = 0; x < w2; ++x) {
                const size_t src =
                    (static_cast<size_t>(y % height) * width + (x % width)) * 3;
                const double scale =
                    0.25 + 0.75 * (static_cast<double>(x) / (w2 - 1));
                for (int c = 0; c < 3; ++c) {
                    rgb2[(static_cast<size_t>(y) * w2 + x) * 3 + c] =
                        static_cast<float>(rgb64[src + c] * scale);
                }
            }
        }
        spk_image in2{rgb2.data(), w2, h2, /*color_space=*/SPK_CS_PROPHOTO};
        spk_params p = base;
        p.scan_film = 1;
        p.grain_active = 1;
        ok &= simulate_with_threads(asset_dir, &in2, &p, 1, &r1);
        ok &= simulate_with_threads(asset_dir, &in2, &p, 8, &r8);
        ok &= check_identical("scan_film+grain multi-block 192x160", r1, r8);
    }

    // 6) Spectral 3D-LUT acceleration ON (print route, BOTH LUTs): the PCHIP
    //    LUT apply is chunked over pixels (and its input normalization too), so
    //    the opt-in LUT path must stay byte-identical across worker counts.
    //    The min-chunk override above makes the 4096-px fixture really split.
    {
        spk_params p = base;
        p.scan_film = 0;
        p.use_scanner_lut = 1;
        p.use_enlarger_lut = 1;
        ok &= simulate_with_threads(asset_dir, &in_img, &p, 1, &r1);
        ok &= simulate_with_threads(asset_dir, &in_img, &p, 8, &r8);
        ok &= check_identical("print+scanner_lut+enlarger_lut", r1, r8);
    }

    // 7) Camera optical diffusion filter ON (scan route, spatial branch): the
    //    direct O(w*h*ks^2) convolution and its reflect-pad build are chunked
    //    over rows — byte-identical across worker counts. halation_active
    //    drives the spatial branch (as in test_diffusion_e2e); the non-default
    //    strength makes the filter really run.
    {
        spk_params p = base;
        p.scan_film = 1;
        p.halation_active = 1;
        p.camera_diffusion_active = 1;
        p.camera_diffusion_strength = 0.8f;
        ok &= simulate_with_threads(asset_dir, &in_img, &p, 1, &r1);
        ok &= simulate_with_threads(asset_dir, &in_img, &p, 8, &r8);
        ok &= check_identical("scan_film+diffusion_filter", r1, r8);
    }

    // 8) BIG-CORE PINNING toggled around renders (spk_set_big_cores).
    //
    //    Scope, stated honestly: on a homogeneous host every core reports the same
    //    cpuinfo_max_freq (or none at all), so detect_big_cores returns 0 and the
    //    pin itself is a NO-OP here. This scenario therefore gates the PLUMBING,
    //    not the pinning: that flipping the mode bumps the generation counter, that
    //    a worker whose thread-local latch predates the bump re-evaluates instead
    //    of going stale, that the OFF path's restore branch does not corrupt a
    //    thread's mask, and that none of it perturbs a single output byte. The
    //    pinning's effect on placement can only be observed on a big.LITTLE device,
    //    which is what the on-device sweep in docs/research/perf-lab.md covers.
    //
    //    Output invariance is the real contract: affinity may change how many
    //    workers split the range, and every worker count must be byte-identical.
    {
        spk_params p = base;
        p.scan_film = 1;
        p.grain_active = 1;
        p.halation_active = 1;

        std::vector<float> off_before, on, off_after;
        spk_set_big_cores(0);
        ok &= simulate_with_threads(asset_dir, &in_img, &p, 8, &off_before);

        spk_set_big_cores(1);
        ok &= simulate_with_threads(asset_dir, &in_img, &p, 8, &on);
        ok &= check_identical("big_cores off->on", off_before, on,
                              "8-thread, pinning off vs on");

        // Back off again: the restore branch runs on every worker that had pinned.
        spk_set_big_cores(0);
        ok &= simulate_with_threads(asset_dir, &in_img, &p, 8, &off_after);
        ok &= check_identical("big_cores on->off", off_before, off_after,
                              "8-thread, pinning off vs restored");

        // With pinning off the reported count must be 0 whatever the topology, so
        // parallel_num_threads() cannot be capped by a stale detection result.
        const int n_off = spk_big_core_count();
        const bool count_ok = (n_off == 0);
        std::printf("[big_cores count when off] %d -> %s\n", n_off,
                    count_ok ? "PASS" : "FAIL");
        ok &= count_ok;

        // Restore the default so later work in this process is unaffected.
        spk_set_big_cores(-1);
    }

    std::printf("%s\n", ok ? "ALL PASS" : "FAIL");
    return ok ? 0 : 1;
}

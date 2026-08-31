/*
 * Public cancellation ABI and synchronized active-render regression. Hostile
 * argument checks remain asset-independent; the final race uses the committed
 * 64x64 fixture plus bundled profiles so sanitizer runs exercise real engine
 * work, cooperative cancellation, cleanup, and destroy-after-return ordering.
 */
#include "spektra.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace {

int cancelled(void*) { return 1; }

struct ActiveRenderCancellation {
    std::mutex mutex;
    std::condition_variable entered_condition;
    std::condition_variable cancel_condition;
    int polls = 0;
    int synchronize_on = 6;
    bool callback_entered = false;
    bool cancel_requested = false;
};

int await_active_render_cancel(void* opaque) {
    auto* state = static_cast<ActiveRenderCancellation*>(opaque);
    std::unique_lock<std::mutex> lock(state->mutex);
    if (++state->polls < state->synchronize_on) return 0;
    state->callback_entered = true;
    state->entered_condition.notify_one();
    state->cancel_condition.wait(lock, [&] { return state->cancel_requested; });
    return 1;
}

bool load_f64_rgb(const char* path, std::vector<float>* rgb) {
    constexpr std::size_t kSamples = 64U * 64U * 3U;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::vector<double> source(kSamples);
    input.read(reinterpret_cast<char*>(source.data()),
               static_cast<std::streamsize>(source.size() * sizeof(double)));
    if (!input) return false;
    rgb->assign(source.begin(), source.end());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    static_assert(SPK_ERR_CANCELLED == 7, "append-only status ABI changed");

    spk_image out{reinterpret_cast<float*>(1), 7, 9, SPK_CS_SRGB};
    const spk_status st = spk_simulate_cancellable(
        nullptr, nullptr, nullptr, &out, cancelled, nullptr);
    assert(st == SPK_ERR_BAD_ARGS);
    assert(out.data == nullptr);
    assert(out.width == 0);
    assert(out.height == 0);

    float sample = 0.0f;
    spk_image oversized{
        &sample, std::numeric_limits<std::int32_t>::max(),
        std::numeric_limits<std::int32_t>::max(), SPK_CS_PROPHOTO};
    spk_params params{};
    out = {reinterpret_cast<float*>(1), 7, 9, SPK_CS_SRGB};
    const spk_status overflow_status = spk_simulate_cancellable(
        reinterpret_cast<spk_engine*>(1), &oversized, &params, &out,
        nullptr, nullptr);
    assert(overflow_status == SPK_ERR_BAD_ARGS);
    assert(out.data == nullptr);
    assert(out.width == 0);
    assert(out.height == 0);

    // Geometry is caller-controlled across JNI. Every hostile value must be
    // rejected at the public C boundary before the engine handle is touched,
    // before floating-point-to-integer casts, and before any output allocation.
    // A deliberately non-dereferenceable engine proves validation ordering.
    float pixel[3] = {0.18f, 0.18f, 0.18f};
    spk_image one{pixel, 1, 1, SPK_CS_PROPHOTO};
    spk_params geometry{};
    geometry.film_profile = "unused_because_geometry_must_fail_first";
    geometry.print_profile = "unused_because_geometry_must_fail_first";
    spk_default_params(&geometry);
    geometry.scan_film = 1;

    const auto rejects_geometry = [&](const spk_params& hostile) {
        spk_image rejected{reinterpret_cast<float*>(1), 7, 9, SPK_CS_SRGB};
        const spk_status status = spk_simulate_cancellable(
            reinterpret_cast<spk_engine*>(1), &one, &hostile, &rejected,
            nullptr, nullptr);
        assert(status == SPK_ERR_BAD_ARGS);
        assert(rejected.data == nullptr);
        assert(rejected.width == 0);
        assert(rejected.height == 0);
    };

    for (float hostile : {
             -1.0f,
             std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity(),
             std::numeric_limits<float>::max(),
         }) {
        spk_params candidate = geometry;
        candidate.upscale_factor = hostile;
        rejects_geometry(candidate);
    }

    geometry.crop = 1;
    for (float hostile : {
             -0.01f,
             1.01f,
             std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity(),
         }) {
        spk_params candidate = geometry;
        candidate.crop_center[0] = hostile;
        rejects_geometry(candidate);
    }
    for (float hostile : {
             -0.01f,
             0.0f,
             1.01f,
             std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity(),
         }) {
        spk_params candidate = geometry;
        candidate.crop_size[1] = hostile;
        rejects_geometry(candidate);
    }

    // Real, asset-backed cancellation race. The render-owner thread blocks in
    // the production callback until the cancelling thread has observed that
    // exact poll, eliminating scheduler sleeps and proving engine destruction
    // happens only after the cancelled render has returned.
    const char* asset_dir = argc > 1
        ? argv[1]
        : "engine/spektra-core/src/main/assets/spektra";
    const char* input_path = argc > 2
        ? argv[2]
        : "engine/spektra-core/src/main/cpp/tests/scan_portra_input_rgb.f64";
    std::vector<float> pixels;
    assert(load_f64_rgb(input_path, &pixels));
    spk_engine* engine = nullptr;
    assert(spk_engine_create(asset_dir, &engine) == SPK_OK && engine != nullptr);

    spk_params render_params{};
    render_params.film_profile = "kodak_portra_400";
    render_params.print_profile = "kodak_portra_endura";
    spk_default_params(&render_params);
    render_params.scan_film = 1;
    render_params.grain_active = 0;
    render_params.halation_active = 0;
    render_params.dir_diffusion_size_um = 0.0f;
    render_params.scanner_unsharp[0] = 0.0f;
    render_params.scanner_unsharp[1] = 0.0f;
    render_params.disable_buffer_memos = 1;
    spk_image render_input{pixels.data(), 64, 64, SPK_CS_PROPHOTO};
    spk_image render_output{reinterpret_cast<float*>(1), 7, 9, SPK_CS_SRGB};
    ActiveRenderCancellation active_cancel;
    bool cancelling_thread_observed_callback = false;
    std::thread cancelling_thread([&] {
        std::unique_lock<std::mutex> lock(active_cancel.mutex);
        cancelling_thread_observed_callback =
            active_cancel.entered_condition.wait_for(
                lock, std::chrono::seconds(5),
                [&] { return active_cancel.callback_entered; });
        active_cancel.cancel_requested = true;
        lock.unlock();
        active_cancel.cancel_condition.notify_one();
    });
    const spk_status render_status = spk_simulate_cancellable(
        engine, &render_input, &render_params, &render_output,
        await_active_render_cancel, &active_cancel);
    cancelling_thread.join();
    assert(cancelling_thread_observed_callback);
    assert(active_cancel.polls == active_cancel.synchronize_on);
    assert(render_status == SPK_ERR_CANCELLED);
    assert(render_output.data == nullptr);
    assert(render_output.width == 0);
    assert(render_output.height == 0);
    spk_engine_destroy(engine);
    return 0;
}

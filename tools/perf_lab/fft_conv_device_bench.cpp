/*
 * Spektrafilm for Android — #160: time the diffusion FFT convolution ON THE PHONE. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * WHY A SEPARATE BINARY
 *
 * #160's remaining question is whether the transform-size COST RANKING measured
 * on a 24-core desktop survives on a 4+4 big.LITTLE phone -- 4096 is 5x faster
 * than 2048 at 12 MP but 2x SLOWER at 1536 px, so the ranking is size-dependent
 * and a wrong pick costs multiples either way.
 *
 * Driving that through the app needs a Black Pro-Mist render per data point, and
 * `debug.spektra.fftmax` only moves the CEILING -- the cost model still picks
 * below it, so the app cannot be made to run a transform the model rejects.
 * A shell binary can: it calls the forced-size test seam directly,
 * on the real SoC, with no UI, no APK and no reinstall.
 *
 * BUILD (host: NDK clang, target: arm64)
 *   tools/perf_lab/build_fft_conv_device_bench.sh [/path/to/ndk]
 *
 * RUN
 *   adb push .../fft_conv_bench /data/local/tmp/ && adb shell /data/local/tmp/fft_conv_bench
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <vector>

#include "kernels/fft.h"        // fft_next_pow2
#include "kernels/fft_convolve.h"

namespace {

// ks tracks image width because the PSF radius is a fixed size on the FILM:
// model/diffusion.cpp measures ks = 273 at 640 px and ks = 1725 at 12 MP, i.e.
// ~0.4267 * width, forced odd. Reproduced here so a width alone names a case.
int ks_for_width(int w) {
    int ks = static_cast<int>(std::lround(0.4267 * w));
    if ((ks & 1) == 0) ++ks;
    return ks < 3 ? 3 : ks;
}

double bench_one(int w, int h, int ks, int forced_n, int reps) {
    const int pw = w + ks - 1, ph = h + ks - 1;
    std::vector<double> padded(static_cast<size_t>(pw) * ph);
    for (size_t i = 0; i < padded.size(); ++i)
        padded[i] = 0.5 + 0.5 * std::sin(static_cast<double>(i) * 0.001);

    // A normalised separable-ish blob: only its SIZE matters to the transform cost,
    // but a degenerate kernel would let a compiler skip work, so keep it non-trivial.
    std::vector<double> kern(static_cast<size_t>(ks) * ks);
    const double c = (ks - 1) / 2.0, lam = ks / 8.0;
    double sum = 0.0;
    for (int y = 0; y < ks; ++y)
        for (int x = 0; x < ks; ++x) {
            const double dx = x - c, dy = y - c;
            const double v = std::exp(-std::sqrt(dx * dx + dy * dy) / lam);
            kern[static_cast<size_t>(y) * ks + x] = v;
            sum += v;
        }
    for (double& v : kern) v /= sum;

    std::vector<double> out(static_cast<size_t>(w) * h);
    double best = -1.0;
    for (int r = 0; r < reps; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        // The forced seam, not fft_convolve_same: the point is to time sizes the
        // cost model REJECTED, and the shipping entry point only ever runs its own
        // pick, so it can never disagree with itself.
        if (!spk::fft_convolve_same_forced_n_for_test(
                padded.data(), pw, ph, kern.data(), ks, w, h,
                out.data(), 1, 0, forced_n)) {
            std::fprintf(stderr, "fft_convolve rejected w=%d h=%d ks=%d n=%d\n",
                         w, h, ks, forced_n);
            return -1.0;
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        // Best-of-reps, not mean: a phone's scheduler and thermal governor only ever
        // ADD time, so the minimum is the least contaminated estimate of the work.
        if (best < 0.0 || ms < best) best = ms;
    }
    return best;
}

}  // namespace

int main(int argc, char** argv) {
    // Default sweep: the interactive preview size, the size where the desktop
    // measured 4096 to be the WRONG pick, and 12 MP where it is the right one.
    // Each positional argument is a case: "w", "w:h", or "w:h:ks". The desktop
    // table this compares against (fft_convolve.cpp) used ks = 651 at 1536 px and
    // ks = 1725 at 12 MP, so ks is spellable rather than always re-derived.
    std::vector<std::vector<int>> cases;
    int reps = 3;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--reps" && i + 1 < argc) { reps = std::atoi(argv[++i]); continue; }
        int f[3] = {0, 0, 0};
        int nf = 0;
        for (size_t p = 0; p <= a.size() && nf < 3;) {
            const size_t c = a.find(':', p);
            f[nf++] = std::atoi(a.substr(p, c == std::string::npos ? c : c - p).c_str());
            if (c == std::string::npos) break;
            p = c + 1;
        }
        if (f[0] < 1) continue;
        if (nf < 2 || f[1] < 1) f[1] = static_cast<int>(std::lround(f[0] * 3.0 / 4.0));
        if (nf < 3 || f[2] < 1) f[2] = ks_for_width(f[0]);
        cases.push_back({f[0], f[1], f[2]});
    }
    if (cases.empty()) cases = {{640, 480, 273}, {1536, 1152, 651}};

    std::printf("width height ks n tiles picked best_ms\n");
    for (const std::vector<int>& c : cases) {
        const int w = c[0], h = c[1], ks = c[2];
        // The selector's own candidate set: from the smallest transform that leaves
        // a usable block, up to the padded plane (past that the extra area is all
        // zero padding, which is why the selector never considers it either).
        const int floor_n = spk::fft_next_pow2(ks + 1);
        const int ideal = spk::fft_next_pow2(std::max(h + ks - 1, w + ks - 1));
        const int pick =
            spk::fft_convolve_transform_size(w, h, ks, spk::kFftConvMaxTransform);
        for (int n = floor_n; n <= ideal; n *= 2) {
            const int block = n - ks + 1;
            if (block <= 0) continue;
            const long tiles =
                static_cast<long>(std::ceil(static_cast<double>(h) / block)) *
                static_cast<long>(std::ceil(static_cast<double>(w) / block));
            const double ms = bench_one(w, h, ks, n, reps);
            if (ms < 0.0) return 1;
            std::printf("%d %d %d %d %ld %s %.1f\n", w, h, ks, n, tiles,
                        n == pick ? "MODEL" : "-", ms);
            std::fflush(stdout);
        }
    }
    return 0;
}

/*
 * Spektrafilm for Android — native engine entry (capstone wiring).
 * GPLv3. Port of spektrafilm (GPLv3) — film modeling powered by spektrafilm.
 *
 * Implements the C API for the scan_film route by orchestrating the already-ported,
 * bit-exact stages:
 *   spk_simulate(scan_film) :
 *     load film profile -> build_filming_tc_lut -> expose -> develop(+couplers)
 *     -> scan -> display RGB (output_color_space, CCTF per params).
 *
 * The print (enlarger) route works for ANY (film, paper) pair: the neutral
 * dichroic CC values are resolved natively from neutral_print_filters.json and
 * the midgray exposure factor is computed natively (runtime/print_digest).
 *
 * Honoured spk_params for the scan_film parity case (scan_portra defaults):
 *   - film_profile          -> profile JSON loaded from <asset_dir>/profiles/<id>.json
 *   - scan_film             -> selects this route (false => print route, implemented + parity-gated)
 *   - exposure_compensation_ev -> FilmingParams.exposure_compensation_ev
 *   - density_curve_gamma   -> FilmingParams.density_curve_gamma (broadcast to CMY)
 *   - output_color_space    -> ScanningParams.output_color_space (all six spaces:
 *                              sRGB, Adobe RGB, ProPhoto, Rec.2020, ACES2065-1,
 *                              linear sRGB; per-space XYZ->RGB matrix + CCTF).
 *   - output_cctf_encoding  -> ScanningParams.output_cctf_encoding
 *   - preview_max_size      -> used by spk_simulate_preview for the downscale target.
 *
 * Honoured stochastic/spatial toggles (historical note — today the FULL
 * spk_params surface is honoured; every effect self-gates on its own params,
 * auto-exposure is fully wired with its own golden, and the deterministic
 * parity goldens simply run with the stochastic stages off):
 *   - halation_active -> in-emulsion scatter + halation (per-effect gating for
 *     the rest since E1).
 *   - grain_active    -> stochastic AgX particle grain (model/grain.cpp).
 */
#include "spektra.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef __ANDROID__
#include <android/asset_manager.h>
#endif

#include "io/npy_lut.h"
#include "kernels/lut3d_cache.h"  // spk_engine holds the spectral 3D-LUT memo by value
#include "kernels/parallel.h"     // spk_set_big_cores -> parallel_set_big_cores
#include "model/diffusion.h"
#include "model/color_filters.h"
#include "profiles/profile.h"
#include "runtime/color_reference.h"
#include "runtime/params.h"
#include "runtime/print_digest.h"
#include "runtime/stage_timer.h"
#include "runtime/stages/autoexposure.h"
#include "runtime/stages/crop_resize.h"
#include "runtime/stages/filming.h"
#include "runtime/stages/printing.h"
#include "runtime/stages/scanning.h"

namespace {

// Status detail is caller-thread-local so concurrent preview/export calls cannot
// overwrite each other's diagnostics. A fixed buffer keeps the error path
// allocation-free and gives the public pointer a simple lifetime contract.
thread_local char g_last_error_message[512] = {};

void clear_last_error_message() noexcept { g_last_error_message[0] = '\0'; }

void set_last_error_message(const char* message) noexcept {
    if (!message) {
        clear_last_error_message();
        return;
    }
    std::strncpy(g_last_error_message, message,
                 sizeof(g_last_error_message) - 1);
    g_last_error_message[sizeof(g_last_error_message) - 1] = '\0';
}

// Build the scanning-stage tone curve from the flat spk_params control points.
// Inactive (the default) => an inactive set whose apply() is a strict no-op. Point
// counts are clamped to [0, SPK_TONE_MAX_PTS]; a count < 2 yields an identity curve.
spk::ToneCurveSet build_tone_curve_set(const spk_params* p) {
    spk::ToneCurveSet set;
    if (p->tone_curve_active == 0) { set.active = false; return set; }
    set.active = true;
    auto clampN = [](int n) {
        if (n < 0) return 0;
        return n > SPK_TONE_MAX_PTS ? SPK_TONE_MAX_PTS : n;
    };
    set.master = spk::build_tone_curve_1d(p->tone_curve_master_x, p->tone_curve_master_y,
                                          clampN(p->tone_curve_master_n));
    for (int c = 0; c < 3; ++c) {
        set.rgb[c] = spk::build_tone_curve_1d(p->tone_curve_rgb_x[c], p->tone_curve_rgb_y[c],
                                              clampN(p->tone_curve_rgb_n[c]));
    }
    return set;
}

// D55 standard illuminant (colour SDS_ILLUMINANTS['D55']) aligned to the
// 380..780 @5nm working shape, normalised by mean — the film reference
// illuminant for the bundled negative profiles. Baked at full double precision
// from the Python oracle, identical to tests/test_filming.cpp.
const double kD55Illuminant[SPK_SPECTRAL_SAMPLES] = {
    0.3792826592081565,0.41130471283820924,0.4433384066186183,0.5763969653409493,
    0.7094555240632804,0.7537113757177554,0.7979788675225865,0.8155671347108852,
    0.8331670420495402,0.8118539267472403,0.7905291712945844,0.8934979413460009,
    0.996455071247061,1.0685541625536938,1.1406532538603265,1.1550288395502992,
    1.169404425240272,1.1662033838923025,1.1630023425443328,1.1794498749977185,
    1.1958974074511046,1.1687758571210345,1.141642666640608,1.1567865022540935,
    1.1719303378675792,1.172023459070429,1.1721049401229227,1.167984326896809,
    1.1638637136706955,1.1884360710727462,1.213020068625153,1.2007513501496623,
    1.1884826316741712,1.1935228167784289,1.1985630018826865,1.1812890187540066,
    1.1640150356253267,1.1478119463294223,1.1316088570335177,1.134705137028281,
    1.1378130571734006,1.1010418221979967,1.0642822273729489,1.0816726120051912,
    1.0990513564870772,1.1032534507656848,1.107443904893936,1.1020894357300595,
    1.096734966566183,1.0747816429942894,1.0528283194223955,1.0637817009076298,
    1.0747350823928643,1.054504501073696,1.0342739197545279,1.0427945098153053,
    1.0513034597257263,1.0724419727726824,1.0935921259699946,1.0703467457085567,
    1.047101365447119,0.9872826327663333,0.9274522599351918,0.945855337648428,
    0.9642700555120207,0.9759334861689865,0.9875969168259522,0.9025656184735221,
    0.8175459602714483,0.8703107618363444,0.9230755634012404,0.9562034313151373,
    0.989331299229034,0.9130184734934376,0.8366940076074848,0.72561205275776,
    0.6145184577576788,0.7491600769284603,0.883801696099242,0.8598811871171415,
    0.8359723182853972};

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() &&
           s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

}  // namespace

// Relative (to the spektra/ asset root) paths of the bundled assets. The
// filesystem mode joins these onto asset_dir; the AAssetManager mode passes them
// (prefixed by asset_base) to AAssetManager_open.
namespace {
constexpr char kSpectraLutRel[] = "luts/spectral_upsampling/irradiance_xy_tc.npy";
constexpr char kNeutralFiltersRel[] = "filters/neutral_print_filters.json";
}  // namespace

// Engine: holds asset paths and lazily caches the spectra LUT (shared across calls).
//
// Two asset-I/O modes, both reading the SAME relative paths (relative to the
// spektra/ asset root, e.g. "profiles/foo.json"):
//   - Filesystem mode (default, used by the host parity tests): assets live under
//     `asset_dir` on disk; spk_read_asset() opens asset_dir/<rel> with ifstream.
//     This path is byte-for-byte the historical behavior and is the parity gate.
//   - AAssetManager mode (Android only): assets live in the APK; spk_read_asset()
//     opens <asset_base>/<rel> via AAssetManager_open. Selected only when the
//     engine is created with spk_engine_create_asset_manager.
struct spk_engine {
    std::string asset_dir;        // root containing profiles/ and luts/ (FS mode)
    std::string profiles_dir;     // <asset_dir>/profiles (FS mode)

#ifdef __ANDROID__
    // AAssetManager mode (null in filesystem mode). Not owned: the Java
    // AssetManager (and thus this pointer) must outlive the engine — the Kotlin
    // side keeps it referenced (see SpektraEngine). When non-null, spk_read_asset
    // and spk_engine_list_profiles take the AAsset path instead of the FS path.
    AAssetManager* asset_mgr = nullptr;
    // Subdir inside the APK assets/ where the bundled tree lives (the app stores
    // its assets under assets/spektra/...), prepended to every relative path for
    // the AAsset case. AAssetManager paths are relative to assets/.
    std::string asset_base = "spektra";
    bool use_asset_mgr() const { return asset_mgr != nullptr; }
#else
    bool use_asset_mgr() const { return false; }
#endif

    std::mutex lut_mutex;
    bool lut_loaded = false;
    spk::NdArray spectra_lut;

    // Per-render setup caches (PERF). Every simulate() otherwise re-parses the
    // film/print profile JSON and rebuilds the filming tc_lut from scratch, even
    // on an interactive slider drag that changed nothing about the profile.
    // profile_cache is keyed purely by the profile id (immutable bundled asset —
    // can never go stale). tc_lut_cache is keyed by engine_tc_lut()'s composite
    // key: film id PLUS every build input that IS a live param — spectral
    // Gaussian blur, the hanatos window/surface toggles, the camera UV/IR
    // band-pass triples, and input_gamut_compress (see engine_tc_lut below; a
    // key that dropped any of these would serve stale tc_luts across a settings
    // change). Entries are never evicted, so node references stay valid — which
    // also means non-default continuous params (blur sigma, UV/IR floats) add
    // one ~885 KB entry PER DISTINCT VALUE for the engine's lifetime; the
    // default path keeps the bare film-id key (28 bundled profiles, bounded).
    // Bounding the non-default entries (LRU, like kernels/lut3d_cache) is an
    // open item tracked in docs/AUDIT.md. Guarded by cache_mutex.
    std::mutex cache_mutex;
    std::map<std::string, spk::Profile> profile_cache;   // id -> parsed Profile
    std::map<std::string, spk::NdArray> tc_lut_cache;    // film id -> filming tc_lut

    // FILM-DENSITY (film_density_cmy) memo, one slot PER ROUTE (PERF). Unlike
    // profile_cache/tc_lut_cache above — which are keyed by an IMMUTABLE
    // bundled-asset id and so can never go stale — these single-slot caches are
    // keyed by a CONTENT+PARAM DIGEST (compute_film_cache_key): a 64-bit FNV-1a
    // hash folding the post-preprocess expose-input buffer (image content +
    // auto-exposure + crop/rescale results) plus every filming-side input param,
    // INCLUDING the deterministic spatial shape params (halation/scatter, DIR
    // diffusion, camera diffusion, lens blur, resize pixel size, bw exposure
    // correction). The key is therefore NOT an id; it is a fingerprint of the
    // exact inputs to expose()+develop(). With grain OFF that pair is a PURE
    // deterministic function of those inputs (the spatial effects are fixed
    // convolutions), making the memo byte-identical to a fresh expose+develop (a
    // memo, not an approximation). It lets a downstream-only edit (printing/
    // scanning/tone-curve params) reuse filming and skip expose+develop. Grain
    // (stochastic) and debug-tap renders always bypass. Two slots — the print
    // route and the scan route fold different inputs (bw exposure correction) and
    // route A/B-ing must not thrash a shared slot. Guarded by film_cache_mutex.
    struct FilmCacheEntry {
        int width = 0;
        int height = 0;
        std::vector<float> film_density_cmy;
    };
    struct FilmMemoSlot {
        uint64_t key = 0;      // 0 == empty slot (no valid entry yet)
        bool valid = false;    // guards the all-zero-key edge case
        FilmCacheEntry entry;
        uint64_t hits = 0;     // host-test observability (NOT public ABI)
        uint64_t misses = 0;
    };
    enum FilmMemoRoute { kMemoPrint = 0, kMemoScan = 1 };
    std::mutex film_cache_mutex;
    FilmMemoSlot film_memo[2];

    // SPECTRAL 3D-LUT memo (PERF), shared by the opt-in scanner LUT (scan()) and
    // the opt-in enlarger LUT (print_expose()). Both are pure functions of the
    // profile spectra, the enlarger/scan constants, the domain bounds and the step
    // count, but were rebuilt from scratch on EVERY call — and
    // spk_simulate_preview forces BOTH on, so every interactive frame paid two
    // steps^3 sweeps of 81-band spectral integrals (plus their O(steps^3) PCHIP
    // prepare) to reproduce a LUT that had not changed. Unlike the buffer memos
    // above this one is keyed by RAW KEY BYTES compared exactly, not by a hash:
    // each stage assembles a key from every value its sample function and grid
    // construction consume (see the fold lists at the two call sites), so a hit
    // returns a LUT byte-identical to rebuilding and no param change can silently
    // reuse a stale one. Bounded LRU — several keyed inputs (the filtered
    // illuminant, the midgray factor, grain.density_min) are live user params, so
    // an unbounded map would grow with slider travel. Thread-safe on its own.
    spk::Lut3DCache lut_cache;

    // PRINT-DENSITY (print_expose + print_develop) memo, keyed by the
    // film_density_cmy buffer CONTENT + every printing-stage input
    // (compute_print_density_key). Content-hashing makes it correct
    // independently of the film memo (grain or a film bypass included: both
    // print stages are deterministic functions of film bytes + params). Lets an
    // output-only edit (scanner / output space / tone curve / glare) rerun
    // scan() alone. The entry buffer holds print_density_cmy. Same mutex.
    FilmMemoSlot print_density_memo;
};

// Host-only accessors for the print-route film_density_cmy cache counters. The host
// parity tests compile spektra.cpp directly into their binary and read these to
// assert the cache actually engaged, WITHOUT touching spektra.h / the public ABI /
// JNI. Gated on the HOST build (!__ANDROID__) so they never enter the shipped
// libspektra.so; not declared in any header — tests forward-declare them. The host
// parity job (engine-parity) builds with the host g++ toolchain, so __ANDROID__ is
// undefined and these are available there without needing an extra -D flag.
#ifndef __ANDROID__
uint64_t spk_test_film_cache_hits(spk_engine* eng) {
    if (!eng) return 0;
    std::lock_guard<std::mutex> g(eng->film_cache_mutex);
    return eng->film_memo[spk_engine::kMemoPrint].hits;
}
uint64_t spk_test_film_cache_misses(spk_engine* eng) {
    if (!eng) return 0;
    std::lock_guard<std::mutex> g(eng->film_cache_mutex);
    return eng->film_memo[spk_engine::kMemoPrint].misses;
}
uint64_t spk_test_scan_film_cache_hits(spk_engine* eng) {
    if (!eng) return 0;
    std::lock_guard<std::mutex> g(eng->film_cache_mutex);
    return eng->film_memo[spk_engine::kMemoScan].hits;
}
uint64_t spk_test_scan_film_cache_misses(spk_engine* eng) {
    if (!eng) return 0;
    std::lock_guard<std::mutex> g(eng->film_cache_mutex);
    return eng->film_memo[spk_engine::kMemoScan].misses;
}
uint64_t spk_test_print_density_cache_hits(spk_engine* eng) {
    if (!eng) return 0;
    std::lock_guard<std::mutex> g(eng->film_cache_mutex);
    return eng->print_density_memo.hits;
}
uint64_t spk_test_print_density_cache_misses(spk_engine* eng) {
    if (!eng) return 0;
    std::lock_guard<std::mutex> g(eng->film_cache_mutex);
    return eng->print_density_memo.misses;
}
// Spectral 3D-LUT memo counters (scanner + enlarger share the cache; the kind tag
// in each key keeps the two apart). tests/test_lut_cache_e2e.cpp reads these to
// assert the memo engages AND that a hit renders byte-identically to a rebuild.
uint64_t spk_test_lut_cache_hits(spk_engine* eng) {
    return eng ? eng->lut_cache.hits() : 0;
}
uint64_t spk_test_lut_cache_misses(spk_engine* eng) {
    return eng ? eng->lut_cache.misses() : 0;
}
#endif

// Read a bundled asset by its path relative to the spektra/ asset root (e.g.
// "profiles/kodak_portra_400.json") into `out`. Returns false on open failure.
// In AAssetManager mode (Android) it opens via AAssetManager_open; otherwise it
// reads asset_dir/<rel_path> with std::ifstream (the historical, parity-gated
// behavior). Throws nothing.
static bool spk_read_asset(spk_engine* eng, const std::string& rel_path,
                           std::vector<char>& out,
                           size_t max_bytes = std::numeric_limits<size_t>::max()) {
    if (!eng) return false;
#ifdef __ANDROID__
    if (eng->use_asset_mgr()) {
        std::string full = eng->asset_base.empty()
                               ? rel_path
                               : eng->asset_base + "/" + rel_path;
        AAsset* a = AAssetManager_open(eng->asset_mgr, full.c_str(),
                                       AASSET_MODE_BUFFER);
        if (!a) return false;
        const off_t len = AAsset_getLength(a);
        if (len < 0 || static_cast<uintmax_t>(len) > max_bytes ||
            static_cast<uintmax_t>(len) > std::numeric_limits<size_t>::max() ||
            static_cast<uintmax_t>(len) >
                static_cast<uintmax_t>(std::numeric_limits<int>::max())) {
            AAsset_close(a);
            return false;
        }
        const size_t size = static_cast<size_t>(len);
        out.resize(size);
        bool ok = true;
        if (size > 0) {
            const int read = AAsset_read(a, out.data(), size);
            ok = read >= 0 && static_cast<size_t>(read) == size;
        }
        AAsset_close(a);
        return ok;
    }
#endif
    // Filesystem mode: read asset_dir/<rel_path> — historical ifstream behavior.
    std::string path = eng->asset_dir;
    if (!path.empty() && path.back() != '/') path += '/';
    path += rel_path;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    std::streamsize size = in.tellg();
    if (size < 0) return false;
    const uintmax_t unsigned_size = static_cast<uintmax_t>(size);
    if (unsigned_size > max_bytes ||
        unsigned_size > std::numeric_limits<size_t>::max()) {
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0 && !in.read(out.data(), size)) return false;
    return true;
}

// Lazily load + cache the Hanatos2025 spectra LUT (shared across calls), reading
// it through spk_read_asset so it works in both filesystem and AAssetManager
// modes. Throws std::runtime_error if the asset can't be read or parsed.
static const spk::NdArray& engine_spectra(spk_engine* eng) {
    std::lock_guard<std::mutex> g(eng->lut_mutex);
    if (!eng->lut_loaded) {
        std::vector<char> buf;
        if (!spk_read_asset(eng, kSpectraLutRel, buf,
                            spk::kMaxNpyFileBytes))
            throw std::runtime_error("spektra: cannot read spectra LUT asset");
        eng->spectra_lut = spk::parse_npy(buf.data(), buf.size(), kSpectraLutRel);
        if (eng->spectra_lut.shape != std::vector<int>({192, 192, 81}) ||
            eng->spectra_lut.data.size() != 2'985'984u) {
            eng->spectra_lut = {};
            throw std::runtime_error("spektra: spectra LUT shape must be 192x192x81");
        }
        eng->lut_loaded = true;
    }
    return eng->spectra_lut;
}

class ProfileAssetNotFound final : public std::runtime_error {
 public:
    using std::runtime_error::runtime_error;
};

class ProfileInvalid final : public std::runtime_error {
 public:
    using std::runtime_error::runtime_error;
};

// Load a film/print profile by id (e.g. "kodak_portra_400") through spk_read_asset.
// Asset absence and invalid content stay distinct so the additive C status and
// JNI diagnostic do not collapse a rejected viewing-illuminant ID into "not found".
static spk::Profile load_engine_profile(spk_engine* eng, const std::string& id) {
    {
        std::lock_guard<std::mutex> g(eng->cache_mutex);
        auto it = eng->profile_cache.find(id);
        if (it != eng->profile_cache.end()) return it->second;  // copy of cached parse
    }
    std::vector<char> buf;
    std::string rel = std::string("profiles/") + id + ".json";
    if (!spk_read_asset(eng, rel, buf))
        throw ProfileAssetNotFound("spektra: cannot read profile asset " + rel);
    spk::Profile parsed;
    try {
        parsed = spk::load_profile_string(std::string(buf.data(), buf.size()));
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        throw ProfileInvalid("profile '" + id + "': " + e.what());
    }
    std::lock_guard<std::mutex> g(eng->cache_mutex);
    // Insert if still absent (another thread may have raced us); either way the
    // stored value equals a fresh parse, so the result is identical.
    return eng->profile_cache.emplace(id, std::move(parsed)).first->second;
}

// Cached filming tc_lut, keyed by (film id, spectral_gaussian_blur, apply_window,
// apply_surface, camera UV/IR band-pass triples, input_gamut_compress) — every
// live param the build consumes. For the DEFAULT values the key is just the film
// id and the cached LUT is byte-identical to rebuilding it (build_filming_tc_lut
// is a pure function of film profile, the immutable spectra LUT, the D55
// constant, and those inputs). A non-default value gets its own cache slot so
// distinct adaptations never collide. Returns a reference into the never-evicted
// cache map (node references stay valid; see the growth note on tc_lut_cache
// above). Throws on build failure (caller maps to SPK_ERR_ASSET_IO, as the
// inline build did).
static const spk::NdArray& engine_tc_lut(spk_engine* eng,
                                         const std::string& film_id,
                                         const spk::Profile& film,
                                         float spectral_gaussian_blur,
                                         bool apply_window, bool apply_surface,
                                         const float* filter_uv,
                                         const float* filter_ir,
                                         spk::InputGamutCompress input_gamut_compress =
                                             spk::InputGamutCompress::kOff) {
    // Compose a key that folds the blur sigma's exact IEEE-754 bytes plus the
    // window/surface toggles and the camera UV/IR band-pass so distinct adaptations
    // (or float jitter) never alias. The DEFAULT (blur==0, window on, surface off,
    // band-pass amplitudes 0) keeps the bare film-id key, preserving the existing
    // cache behaviour for the default path.
    const bool band_pass_on =
        filter_uv != nullptr && filter_ir != nullptr &&
        (filter_uv[0] > 0.0f || filter_ir[0] > 0.0f);
    const bool gamut_in_on =
        input_gamut_compress != spk::InputGamutCompress::kOff;
    std::string key = film_id;
    if (spectral_gaussian_blur > 0.0f || !apply_window || apply_surface ||
        band_pass_on || gamut_in_on) {
        key.push_back('|');
        const unsigned char* b =
            reinterpret_cast<const unsigned char*>(&spectral_gaussian_blur);
        for (size_t i = 0; i < sizeof(float); ++i) {
            key.push_back(static_cast<char>(b[i]));
        }
        key.push_back(apply_window ? 'W' : 'w');
        key.push_back(apply_surface ? 'S' : 's');
        // Fold the band-pass triples' exact bytes only when active, so the default
        // (off) key is unchanged.
        if (band_pass_on) {
            key.push_back('B');
            const unsigned char* fu =
                reinterpret_cast<const unsigned char*>(filter_uv);
            const unsigned char* fr =
                reinterpret_cast<const unsigned char*>(filter_ir);
            for (size_t i = 0; i < 3 * sizeof(float); ++i)
                key.push_back(static_cast<char>(fu[i]));
            for (size_t i = 0; i < 3 * sizeof(float); ++i)
                key.push_back(static_cast<char>(fr[i]));
        }
        // Fold the input gamut-compression mode only when active, so the default
        // (kOff) key is byte-identical to the pre-feature film-id key.
        if (gamut_in_on) {
            key.push_back('G');
            const int32_t gmode = static_cast<int32_t>(input_gamut_compress);
            const unsigned char* gb =
                reinterpret_cast<const unsigned char*>(&gmode);
            for (size_t i = 0; i < sizeof(int32_t); ++i)
                key.push_back(static_cast<char>(gb[i]));
        }
    }
    {
        std::lock_guard<std::mutex> g(eng->cache_mutex);
        auto it = eng->tc_lut_cache.find(key);
        if (it != eng->tc_lut_cache.end()) return it->second;
    }
    spk::NdArray lut = spk::build_filming_tc_lut(film, engine_spectra(eng),
                                                 kD55Illuminant,
                                                 spectral_gaussian_blur,
                                                 apply_window, apply_surface,
                                                 filter_uv, filter_ir,
                                                 input_gamut_compress);
    std::lock_guard<std::mutex> g(eng->cache_mutex);
    return eng->tc_lut_cache.emplace(key, std::move(lut)).first->second;
}

// FNV-1a 64-bit over raw bytes. Used to build the print-route film_density_cmy
// cache key by folding the IEEE-754 bytes of every filming-side input.
static inline uint64_t fnv1a64(uint64_t h, const void* data, size_t n) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 0x100000001b3ULL;
    }
    return h;
}

// Compute the film_density_cmy memo key (shared by BOTH routes). The key is a
// 64-bit FNV-1a hash folding EVERY input that the filming step (expose +
// develop, grain-OFF) consumes, so two calls collide IFF their film_density_cmy
// is byte-identical:
//   - the POST-preprocess `rgb` float64 buffer (the actual expose() input): this
//     already folds in image content + auto-exposure + crop/rescale geometry,
//   - width, height, input color_space,
//   - film_profile string bytes,
//   - exposure_compensation_ev, density_curve_gamma (the FILM gamma, not print gamma),
//     rgb_to_raw_method, spectral_gaussian_blur (blurs the filming tc_lut),
//   - the DIR pointwise params (active, amount, inhibition same/inter-layer),
//   - the DETERMINISTIC SPATIAL SHAPE params (Option A): the halation/scatter
//     user set, the DIR diffusion trio, the camera-diffusion filter set, the
//     camera lens blur, plus the two caller-supplied locals `resize_pixel_size_um`
//     (µm->px conversion for every spatial kernel) and `bw_exposure_correction`
//     (multiplies raw inside expose on the scan route; print passes 1.0) — so a
//     spatial-ON render memoizes too and any shape edit changes the key,
//   - DEFENSIVELY: the geometry params (crop, crop_center, crop_size, upscale_factor,
//     film_format_mm) and the branch toggles (grain/halation/glare/diffusion) — so a
//     future change that begins to influence filming can never silently reuse a
//     stale entry.
// IMPORTANT: the rgb buffer is the post-preprocess expose input, so geometry params
// are already reflected in it; folding them too is belt-and-suspenders, not required.
static uint64_t compute_film_cache_key(const std::vector<double>& rgb, int width,
                                       int height, int input_color_space,
                                       const spk_params* p,
                                       double resize_pixel_size_um,
                                       double bw_exposure_correction) {
    uint64_t h = 0xcbf29ce484222325ULL;  // FNV offset basis
    // 1) The expose input buffer (content + AE + crop/rescale already baked in).
    h = fnv1a64(h, rgb.data(), rgb.size() * sizeof(double));
    // 2) Geometry / domain scalars.
    h = fnv1a64(h, &width, sizeof(width));
    h = fnv1a64(h, &height, sizeof(height));
    h = fnv1a64(h, &input_color_space, sizeof(input_color_space));
    // 3) Film profile id bytes (NULL-safe).
    if (p->film_profile) h = fnv1a64(h, p->film_profile, std::strlen(p->film_profile));
    // 4) Filming pointwise params that change film_density_cmy.
    h = fnv1a64(h, &p->exposure_compensation_ev, sizeof(p->exposure_compensation_ev));
    h = fnv1a64(h, &p->density_curve_gamma, sizeof(p->density_curve_gamma));
    int32_t rgb2raw = static_cast<int32_t>(p->rgb_to_raw_method);
    h = fnv1a64(h, &rgb2raw, sizeof(rgb2raw));
    // Spectral-domain blur of the filming tc_lut changes film_density_cmy, so it
    // MUST be part of the print-route memo key (blur defaults to 0 -> no-op, key
    // unchanged from before for the default path).
    h = fnv1a64(h, &p->spectral_gaussian_blur, sizeof(p->spectral_gaussian_blur));
    // hanatos2025 window/surface adaptation toggles also change the filming tc_lut
    // (and therefore film_density_cmy), so they MUST be part of the print-route memo
    // key — otherwise toggling them returns a stale cached film_density_cmy. The
    // DEFAULTS (window=1, surface=0) keep the key unchanged from before for the
    // default path... but fold them ALWAYS so the digest is honest.
    h = fnv1a64(h, &p->apply_hanatos_window, sizeof(p->apply_hanatos_window));
    h = fnv1a64(h, &p->apply_hanatos_surface, sizeof(p->apply_hanatos_surface));
    // Camera UV/IR band-pass cut filter also modifies the filming tc_lut (and thus
    // film_density_cmy) on the print route, so it MUST be part of the memo key.
    // Defaults (amp 0/0) keep the key unchanged for the default path; fold ALWAYS
    // so the digest is honest.
    h = fnv1a64(h, p->camera_filter_uv, sizeof(p->camera_filter_uv));
    h = fnv1a64(h, p->camera_filter_ir, sizeof(p->camera_filter_ir));
    // Input gamut compression bakes a radial-to-locus chromaticity remap into the
    // filming tc_lut (build_filming_tc_lut), so it changes film_density_cmy and MUST
    // be part of the print-route memo key — otherwise toggling it returns a stale
    // cached negative. Default kOff (0) keeps the key unchanged for the default path;
    // fold ALWAYS so the digest is honest (parity-transparent: a key change only ever
    // forces a one-time recompute to the correct value).
    h = fnv1a64(h, &p->input_gamut_compress, sizeof(p->input_gamut_compress));
    // Highlight boost (numba_boost_hightlights.boost_highlights) runs in expose()
    // BEFORE the log10, so it changes film_density_cmy and MUST be part of the
    // print-route memo key — otherwise a boost edit returns a stale cached negative.
    // boost_ev defaults to 0 (a strict no-op); fold all three ALWAYS so the digest is
    // honest. (Adding to the hash only forces a one-time recompute; the cached value
    // is always the correct recompute, so this is parity-transparent.)
    h = fnv1a64(h, &p->halation_boost_ev, sizeof(p->halation_boost_ev));
    h = fnv1a64(h, &p->halation_boost_range, sizeof(p->halation_boost_range));
    h = fnv1a64(h, &p->halation_protect_ev, sizeof(p->halation_protect_ev));
    // 5) DIR-coupler pointwise params.
    h = fnv1a64(h, &p->dir_couplers_active, sizeof(p->dir_couplers_active));
    h = fnv1a64(h, &p->dir_amount, sizeof(p->dir_amount));
    h = fnv1a64(h, &p->dir_inhibition_samelayer, sizeof(p->dir_inhibition_samelayer));
    h = fnv1a64(h, &p->dir_inhibition_interlayer, sizeof(p->dir_inhibition_interlayer));
    // 6) Deterministic spatial shape params (Option A). Every field a spatial
    //    kernel reads must be here, or a shape-only edit would alias a stale
    //    entry (the key-completeness scenarios in test_simulate_e2e assert this
    //    per param). Folding a field that is currently inert only forces a
    //    one-time recompute — parity-transparent.
    //    DIR diffusion trio (spatial inhibitor diffusion in develop):
    h = fnv1a64(h, &p->dir_diffusion_size_um, sizeof(p->dir_diffusion_size_um));
    h = fnv1a64(h, &p->dir_diffusion_tail_um, sizeof(p->dir_diffusion_tail_um));
    h = fnv1a64(h, &p->dir_diffusion_tail_weight, sizeof(p->dir_diffusion_tail_weight));
    //    Halation/scatter user set (apply_user_halation, minus the boost trio
    //    already folded above):
    h = fnv1a64(h, &p->halation_scatter_amount, sizeof(p->halation_scatter_amount));
    h = fnv1a64(h, &p->halation_scatter_spatial_scale,
                sizeof(p->halation_scatter_spatial_scale));
    h = fnv1a64(h, p->halation_scatter_core_um, sizeof(p->halation_scatter_core_um));
    h = fnv1a64(h, p->halation_scatter_tail_um, sizeof(p->halation_scatter_tail_um));
    h = fnv1a64(h, p->halation_scatter_tail_weight,
                sizeof(p->halation_scatter_tail_weight));
    h = fnv1a64(h, &p->halation_halation_amount, sizeof(p->halation_halation_amount));
    h = fnv1a64(h, &p->halation_halation_spatial_scale,
                sizeof(p->halation_halation_spatial_scale));
    h = fnv1a64(h, &p->halation_n_bounces, sizeof(p->halation_n_bounces));
    h = fnv1a64(h, &p->halation_bounce_decay, sizeof(p->halation_bounce_decay));
    h = fnv1a64(h, &p->halation_renormalize, sizeof(p->halation_renormalize));
    //    Camera optical diffusion filter (9 shape fields; active folded below):
    h = fnv1a64(h, &p->camera_diffusion_strength, sizeof(p->camera_diffusion_strength));
    h = fnv1a64(h, &p->camera_diffusion_family,
                sizeof(p->camera_diffusion_family));
    h = fnv1a64(h, &p->camera_diffusion_spatial_scale,
                sizeof(p->camera_diffusion_spatial_scale));
    h = fnv1a64(h, &p->camera_diffusion_halo_warmth,
                sizeof(p->camera_diffusion_halo_warmth));
    h = fnv1a64(h, &p->camera_diffusion_core_intensity,
                sizeof(p->camera_diffusion_core_intensity));
    h = fnv1a64(h, &p->camera_diffusion_core_size, sizeof(p->camera_diffusion_core_size));
    h = fnv1a64(h, &p->camera_diffusion_halo_intensity,
                sizeof(p->camera_diffusion_halo_intensity));
    h = fnv1a64(h, &p->camera_diffusion_halo_size, sizeof(p->camera_diffusion_halo_size));
    h = fnv1a64(h, &p->camera_diffusion_bloom_intensity,
                sizeof(p->camera_diffusion_bloom_intensity));
    h = fnv1a64(h, &p->camera_diffusion_bloom_size,
                sizeof(p->camera_diffusion_bloom_size));
    //    Caller-supplied locals: the µm->px scale every spatial kernel divides by,
    //    and the scan-route bw exposure correction applied inside expose().
    h = fnv1a64(h, &resize_pixel_size_um, sizeof(resize_pixel_size_um));
    h = fnv1a64(h, &bw_exposure_correction, sizeof(bw_exposure_correction));
    // 7) DEFENSIVE: geometry params (already folded via `rgb`, repeated for safety)
    //    + the branch toggles. If any of these ever start influencing the filming
    //    output differently, the key changes and the memo can't go stale.
    h = fnv1a64(h, &p->crop, sizeof(p->crop));
    h = fnv1a64(h, p->crop_center, sizeof(p->crop_center));
    h = fnv1a64(h, p->crop_size, sizeof(p->crop_size));
    h = fnv1a64(h, &p->upscale_factor, sizeof(p->upscale_factor));
    h = fnv1a64(h, &p->film_format_mm, sizeof(p->film_format_mm));
    h = fnv1a64(h, &p->grain_active, sizeof(p->grain_active));
    h = fnv1a64(h, &p->halation_active, sizeof(p->halation_active));
    h = fnv1a64(h, &p->glare_active, sizeof(p->glare_active));
    h = fnv1a64(h, &p->camera_diffusion_active, sizeof(p->camera_diffusion_active));
    h = fnv1a64(h, &p->lens_blur_um, sizeof(p->lens_blur_um));
    h = fnv1a64(h, &p->auto_exposure, sizeof(p->auto_exposure));
    return h;
}

// Compute the print-density (print_expose + print_develop) memo key. Keyed by
// the film_density_cmy buffer CONTENT — not by how it was produced — so this
// memo is correct independently of the film memo's key completeness (grain, a
// film-memo bypass, anything: identical film bytes + identical printing inputs
// => byte-identical print_density_cmy, both stages being deterministic). Folds
// every input the printing stage consumes AFTER film density exists:
// profiles (enlarger illuminant/paper curves + the film-derived midgray), the
// dichroic neutral CC + shifts, print gamma/exposure/midgray toggles, the s023
// morph set, the preflash trio, the enlarger LUT settings, the enlarger
// diffusion filter (+ the µm->px scale), and the print-route b/w correction
// inputs (they set pparams.bw_exposure_correction inside print_expose).
static uint64_t compute_print_density_key(const std::vector<float>& film_density_cmy,
                                          int width, int height,
                                          const spk_params* p,
                                          double resize_pixel_size_um) {
    uint64_t h = 0xcbf29ce484222325ULL;  // FNV offset basis
    h = fnv1a64(h, film_density_cmy.data(),
                film_density_cmy.size() * sizeof(float));
    h = fnv1a64(h, &width, sizeof(width));
    h = fnv1a64(h, &height, sizeof(height));
    if (p->film_profile) h = fnv1a64(h, p->film_profile, std::strlen(p->film_profile));
    if (p->print_profile) h = fnv1a64(h, p->print_profile, std::strlen(p->print_profile));
    if (p->enlarger_illuminant) {
        h = fnv1a64(h, p->enlarger_illuminant,
                    std::strlen(p->enlarger_illuminant));
    }
    // tc_lut shape (engine_tc_lut key inputs): the midgray exposure factor inside
    // print_expose is computed FROM the tc_lut directly — not through the film
    // bytes — so these film-side params are genuine printing-stage inputs and
    // must not alias across the content hash.
    h = fnv1a64(h, &p->spectral_gaussian_blur, sizeof(p->spectral_gaussian_blur));
    h = fnv1a64(h, &p->apply_hanatos_window, sizeof(p->apply_hanatos_window));
    h = fnv1a64(h, &p->apply_hanatos_surface, sizeof(p->apply_hanatos_surface));
    h = fnv1a64(h, p->camera_filter_uv, sizeof(p->camera_filter_uv));
    h = fnv1a64(h, p->camera_filter_ir, sizeof(p->camera_filter_ir));
    h = fnv1a64(h, &p->input_gamut_compress, sizeof(p->input_gamut_compress));
    // Dichroic neutral CC (database flag + explicit values) + user shifts.
    h = fnv1a64(h, &p->neutral_print_filters_from_database,
                sizeof(p->neutral_print_filters_from_database));
    h = fnv1a64(h, &p->c_filter_neutral, sizeof(p->c_filter_neutral));
    h = fnv1a64(h, &p->m_filter_neutral, sizeof(p->m_filter_neutral));
    h = fnv1a64(h, &p->y_filter_neutral, sizeof(p->y_filter_neutral));
    h = fnv1a64(h, &p->m_filter_shift, sizeof(p->m_filter_shift));
    h = fnv1a64(h, &p->y_filter_shift, sizeof(p->y_filter_shift));
    // Print gamma / exposure / midgray balance.
    h = fnv1a64(h, &p->print_density_curve_gamma, sizeof(p->print_density_curve_gamma));
    h = fnv1a64(h, &p->exposure_compensation_ev, sizeof(p->exposure_compensation_ev));
    h = fnv1a64(h, &p->normalize_print_exposure, sizeof(p->normalize_print_exposure));
    h = fnv1a64(h, &p->print_exposure_compensation,
                sizeof(p->print_exposure_compensation));
    h = fnv1a64(h, &p->print_exposure, sizeof(p->print_exposure));
    // s023 print density-curve morph (consumed in print_develop).
    h = fnv1a64(h, &p->print_morph_active, sizeof(p->print_morph_active));
    h = fnv1a64(h, &p->print_morph_gamma_factor, sizeof(p->print_morph_gamma_factor));
    h = fnv1a64(h, &p->print_morph_gamma_factor_fast,
                sizeof(p->print_morph_gamma_factor_fast));
    h = fnv1a64(h, &p->print_morph_gamma_factor_slow,
                sizeof(p->print_morph_gamma_factor_slow));
    h = fnv1a64(h, &p->print_morph_gamma_factor_red,
                sizeof(p->print_morph_gamma_factor_red));
    h = fnv1a64(h, &p->print_morph_gamma_factor_green,
                sizeof(p->print_morph_gamma_factor_green));
    h = fnv1a64(h, &p->print_morph_gamma_factor_blue,
                sizeof(p->print_morph_gamma_factor_blue));
    h = fnv1a64(h, &p->print_morph_developer_exhaustion,
                sizeof(p->print_morph_developer_exhaustion));
    // Enlarger preflash.
    h = fnv1a64(h, &p->preflash_exposure, sizeof(p->preflash_exposure));
    h = fnv1a64(h, &p->preflash_m_filter_shift, sizeof(p->preflash_m_filter_shift));
    h = fnv1a64(h, &p->preflash_y_filter_shift, sizeof(p->preflash_y_filter_shift));
    // Opt-in enlarger 3D-LUT acceleration (changes the print_expose path).
    h = fnv1a64(h, &p->use_enlarger_lut, sizeof(p->use_enlarger_lut));
    h = fnv1a64(h, &p->lut_resolution, sizeof(p->lut_resolution));
    h = fnv1a64(h, p->grain_density_min, sizeof(p->grain_density_min));
    // Enlarger optical diffusion filter (print_expose spatial pass) + µm->px.
    h = fnv1a64(h, &p->enlarger_diffusion_active, sizeof(p->enlarger_diffusion_active));
    h = fnv1a64(h, &p->enlarger_diffusion_family,
                sizeof(p->enlarger_diffusion_family));
    h = fnv1a64(h, &p->enlarger_diffusion_strength,
                sizeof(p->enlarger_diffusion_strength));
    h = fnv1a64(h, &p->enlarger_diffusion_spatial_scale,
                sizeof(p->enlarger_diffusion_spatial_scale));
    h = fnv1a64(h, &p->enlarger_diffusion_halo_warmth,
                sizeof(p->enlarger_diffusion_halo_warmth));
    h = fnv1a64(h, &p->enlarger_diffusion_core_intensity,
                sizeof(p->enlarger_diffusion_core_intensity));
    h = fnv1a64(h, &p->enlarger_diffusion_core_size,
                sizeof(p->enlarger_diffusion_core_size));
    h = fnv1a64(h, &p->enlarger_diffusion_halo_intensity,
                sizeof(p->enlarger_diffusion_halo_intensity));
    h = fnv1a64(h, &p->enlarger_diffusion_halo_size,
                sizeof(p->enlarger_diffusion_halo_size));
    h = fnv1a64(h, &p->enlarger_diffusion_bloom_intensity,
                sizeof(p->enlarger_diffusion_bloom_intensity));
    h = fnv1a64(h, &p->enlarger_diffusion_bloom_size,
                sizeof(p->enlarger_diffusion_bloom_size));
    h = fnv1a64(h, &resize_pixel_size_um, sizeof(resize_pixel_size_um));
    // Print-route b/w correction inputs (drive pparams.bw_exposure_correction).
    h = fnv1a64(h, &p->scanner_white_correction, sizeof(p->scanner_white_correction));
    h = fnv1a64(h, &p->scanner_black_correction, sizeof(p->scanner_black_correction));
    h = fnv1a64(h, &p->scanner_white_level, sizeof(p->scanner_white_level));
    h = fnv1a64(h, &p->scanner_black_level, sizeof(p->scanner_black_level));
    return h;
}

namespace {

// Apply the user-controllable DIR-coupler params from spk_params onto the
// digested struct. The per-channel gamma matrices stay film-specific (baked by
// digest_filming_params, mirroring _apply_film_specifics which overwrites them
// regardless of user input), so they are NOT taken from spk_params. The
// diffusion trio is copied unconditionally, matching the oracle's per-effect
// self-gating: diffusion_size_um <= 0 delegates to the pointwise path
// (model/couplers.cpp), so a zero size is a strict no-op. All values default to
// the schema defaults, so default params are bit-exact.
void apply_user_dir_couplers(spk::DirCouplersParams& dc, const spk_params* p) {
    dc.active = (p->dir_couplers_active != 0);
    dc.amount = p->dir_amount;
    dc.inhibition_samelayer = p->dir_inhibition_samelayer;
    dc.inhibition_interlayer = p->dir_inhibition_interlayer;
    dc.diffusion_size_um = p->dir_diffusion_size_um;
    dc.diffusion_tail_um = p->dir_diffusion_tail_um;
    dc.diffusion_tail_weight = p->dir_diffusion_tail_weight;
}

// Apply the user-controllable halation params from spk_params. The preset-driven
// fields (halation_first_sigma_um and halation_strength) stay baked by
// digest_halation_params (mirroring _apply_halation_preset, which overwrites them
// from the film's use/antihalation tags regardless of user input). Everything
// else (scatter geometry, amounts, boost/protect, bounce model) is user-driven.
void apply_user_halation(spk::HalationParams& h, const spk_params* p) {
    h.boost_ev = p->halation_boost_ev;
    h.boost_range = p->halation_boost_range;
    h.protect_ev = p->halation_protect_ev;
    h.scatter_amount = p->halation_scatter_amount;
    h.scatter_spatial_scale = p->halation_scatter_spatial_scale;
    h.scatter_core_um[0] = p->halation_scatter_core_um[0];
    h.scatter_core_um[1] = p->halation_scatter_core_um[1];
    h.scatter_core_um[2] = p->halation_scatter_core_um[2];
    h.scatter_tail_um[0] = p->halation_scatter_tail_um[0];
    h.scatter_tail_um[1] = p->halation_scatter_tail_um[1];
    h.scatter_tail_um[2] = p->halation_scatter_tail_um[2];
    h.scatter_tail_weight[0] = p->halation_scatter_tail_weight[0];
    h.scatter_tail_weight[1] = p->halation_scatter_tail_weight[1];
    h.scatter_tail_weight[2] = p->halation_scatter_tail_weight[2];
    h.halation_amount = p->halation_halation_amount;
    h.halation_spatial_scale = p->halation_halation_spatial_scale;
    h.halation_n_bounces = p->halation_n_bounces;
    h.halation_bounce_decay = p->halation_bounce_decay;
    h.halation_renormalize = (p->halation_renormalize != 0);
}

// Apply the full user grain params from spk_params onto the digested struct.
// density_max_curves / density_max_layers stay film-derived (filled by develop()
// from the profile's density curves). All other fields are user-driven and equal
// the schema defaults for a default-constructed SpektraParams.
void apply_user_grain(spk::GrainParams& g, const spk_params* p) {
    g.sublayers_active = (p->grain_sublayers_active != 0);
    g.agx_particle_area_um2 = p->grain_particle_area_um2;
    g.agx_particle_scale[0] = p->grain_particle_scale[0];
    g.agx_particle_scale[1] = p->grain_particle_scale[1];
    g.agx_particle_scale[2] = p->grain_particle_scale[2];
    g.agx_particle_scale_layers[0] = p->grain_particle_scale_layers[0];
    g.agx_particle_scale_layers[1] = p->grain_particle_scale_layers[1];
    g.agx_particle_scale_layers[2] = p->grain_particle_scale_layers[2];
    g.density_min[0] = p->grain_density_min[0];
    g.density_min[1] = p->grain_density_min[1];
    g.density_min[2] = p->grain_density_min[2];
    g.uniformity[0] = p->grain_uniformity[0];
    g.uniformity[1] = p->grain_uniformity[1];
    g.uniformity[2] = p->grain_uniformity[2];
    g.blur = p->grain_blur;
    g.blur_dye_clouds_um = p->grain_blur_dye_clouds_um;
    g.micro_structure[0] = p->grain_micro_structure[0];
    g.micro_structure[1] = p->grain_micro_structure[1];
    g.n_sub_layers = p->grain_n_sub_layers > 0 ? p->grain_n_sub_layers : 1;
}

// Apply the camera/enlarger optical diffusion-filter params from spk_params onto
// the digested struct. The family selector is validated at each route entry and
// then copied into the native model. active defaults to 0, so default params
// leave the diffusion filter inactive (a strict no-op). `is_camera` selects the
// camera (filming) vs enlarger (printing) field group.
void apply_user_diffusion_filter(spk::DiffusionFilterParams& d,
                                 const spk_params* p, bool is_camera) {
    if (is_camera) {
        const int32_t family = p->camera_diffusion_family;
        d.family = family == SPK_DIFFUSION_DEFAULT
                       ? spk::DiffusionFamily::kBlackProMist
                       : static_cast<spk::DiffusionFamily>(family - 1);
        d.active = (p->camera_diffusion_active != 0);
        d.strength = p->camera_diffusion_strength;
        d.spatial_scale = p->camera_diffusion_spatial_scale;
        d.halo_warmth = p->camera_diffusion_halo_warmth;
        d.core_intensity = p->camera_diffusion_core_intensity;
        d.core_size = p->camera_diffusion_core_size;
        d.halo_intensity = p->camera_diffusion_halo_intensity;
        d.halo_size = p->camera_diffusion_halo_size;
        d.bloom_intensity = p->camera_diffusion_bloom_intensity;
        d.bloom_size = p->camera_diffusion_bloom_size;
    } else {
        const int32_t family = p->enlarger_diffusion_family;
        d.family = family == SPK_DIFFUSION_DEFAULT
                       ? spk::DiffusionFamily::kBlackProMist
                       : static_cast<spk::DiffusionFamily>(family - 1);
        d.active = (p->enlarger_diffusion_active != 0);
        d.strength = p->enlarger_diffusion_strength;
        d.spatial_scale = p->enlarger_diffusion_spatial_scale;
        d.halo_warmth = p->enlarger_diffusion_halo_warmth;
        d.core_intensity = p->enlarger_diffusion_core_intensity;
        d.core_size = p->enlarger_diffusion_core_size;
        d.halo_intensity = p->enlarger_diffusion_halo_intensity;
        d.halo_size = p->enlarger_diffusion_halo_size;
        d.bloom_intensity = p->enlarger_diffusion_bloom_intensity;
        d.bloom_size = p->enlarger_diffusion_bloom_size;
    }
}

// Build the crop/resize params from spk_params (mirrors IOParams' crop fields).
spk::CropResizeParams build_crop_resize(const spk_params* p) {
    spk::CropResizeParams cr;
    cr.crop = (p->crop != 0);
    cr.crop_center[0] = static_cast<double>(p->crop_center[0]);
    cr.crop_center[1] = static_cast<double>(p->crop_center[1]);
    cr.crop_size[0] = static_cast<double>(p->crop_size[0]);
    cr.crop_size[1] = static_cast<double>(p->crop_size[1]);
    cr.upscale_factor = p->upscale_factor != 0.0f
                            ? static_cast<double>(p->upscale_factor) : 1.0;
    return cr;
}

// Promote the incoming float32 RGB to float64 and apply pipeline._preprocess:
//   image = np.double(image[..., 0:3])
//   image = self._filming_stage.auto_exposure(image)   # metering + global gain
//   image = self._resize_service.crop_and_rescale(image)
// This runs BEFORE the filming stage. With default *parity* params (auto-exposure
// off, crop off, upscale 1.0) it is a pure passthrough that leaves width/height
// and pixel_size_um unchanged, so the existing goldens stay byte-identical.
//
// AUTO-EXPOSURE: when p->auto_exposure is set, FilmingStage.auto_exposure meters
// the luminance of small_preview(image) (a max-256 nearest downscale of the
// ORIGINAL, pre-crop image) under the chosen metering pattern, computes
// ev = -log2(metered/0.184), and scales the FULL-resolution float64 image by
// 2**ev — a single global gain applied before crop/rescale, exactly like the
// oracle. input_color_space is ProPhoto RGB (the engine's input space) and
// cctf decoding follows io.input_cctf_decoding (default off).
//
// `pixel_size_um` is computed here as film_format_mm*1000/max(h,w) on the
// ORIGINAL geometry (matching ResizingService, which sets it from the incoming
// shape before crop) and then divided by upscale_factor when a rescale runs.
// Result of the geometry preprocess, handed to the filming stage. Two forms:
//   - materialized: `rgb` holds the float64 image (crop/rescale ran, or the
//     film-density memo is active and its key must hash the buffer bytes);
//   - direct: `direct` is set and filming reads the caller's FLOAT32 pixels
//     through expose_f32_gain with `gain` (= 2^ae_ev, 1.0 when AE is off)
//     folded into each load. Value-identical to materializing: float->double
//     widening is exact and the gain multiply is the same double op
//     apply_auto_exposure performed in place, in the same order — gated by
//     test_simulate_e2e scenario G (flag-on vs flag-off byte identity).
// The direct form skips the ~288 MB (12 MP) float64 image entirely
// (EXPORT_FASTPATH item 4); it engages only for one-shot renders
// (disable_buffer_memos, where no memo key ever needs the bytes) with no-op
// geometry (crop off, upscale 1.0 — anything else must materialize to run the
// crop/rescale math).
struct PreprocessedInput {
    std::vector<double> rgb;  // materialized float64 image (empty when direct)
    bool direct = false;
    double gain = 1.0;
};

void preprocess_geometry(const spk_image* in, const spk_params* p,
                         PreprocessedInput* out, int* width, int* height,
                         double* pixel_size_um) {
    const int w = in->width, h = in->height;
    const double fmm = p->film_format_mm > 0.0f ? p->film_format_mm : 35.0;
    const int longest = w > h ? w : h;
    *pixel_size_um = fmm * 1000.0 / static_cast<double>(longest);

    // Method resolution shared by both forms (NULL => schema default).
    spk::AeMethod method = spk::AeMethod::kCenterWeighted;
    bool known = true;
    if (p->auto_exposure != 0 && p->auto_exposure_method != nullptr) {
        known = spk::ae_method_from_string(p->auto_exposure_method, &method);
    }

    // upscale 0 normalizes to 1.0 in build_crop_resize, so both values mean
    // "no rescale" — keep this gate aligned with that mapping.
    const bool geometry_noop =
        (p->crop == 0) &&
        (p->upscale_factor == 1.0f || p->upscale_factor == 0.0f);
    if (geometry_noop && p->disable_buffer_memos != 0) {
        // Direct form: meter straight off the float32 frame (byte-identical EV —
        // see measure_auto_exposure_ev_f32) and let filming fold the gain into
        // each pixel load. No float64 image is ever built.
        out->direct = true;
        out->gain = 1.0;
        if (p->auto_exposure != 0) {
            const double ev = spk::measure_auto_exposure_ev_f32(
                in->data, w, h, spk::AeColorSpace::kProPhotoRGB,
                /*apply_cctf_decoding=*/p->input_cctf_decoding != 0, method,
                known);
            out->gain = std::pow(2.0, ev);
        }
        *width = w;
        *height = h;
        return;  // pixel_size_um unchanged: no rescale ran
    }

    std::vector<double> src(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = static_cast<double>(in->data[i]);

    // Auto-exposure (pipeline._preprocess -> FilmingStage.auto_exposure). Runs on
    // the original geometry, BEFORE crop_and_rescale. No-op when off.
    if (p->auto_exposure != 0) {
        spk::apply_auto_exposure(src.data(), w, h,
                                 spk::AeColorSpace::kProPhotoRGB,
                                 /*apply_cctf_decoding=*/p->input_cctf_decoding != 0,
                                 method, known);
    }

    if (geometry_noop) {
        // crop_and_rescale would be a pure copy here — move instead (identical
        // bytes; the copy briefly doubled full-res float64 residency,
        // EXPORT_FASTPATH item 4).
        out->rgb = std::move(src);
        *width = w;
        *height = h;
        return;
    }
    spk::CropResizeParams cr = build_crop_resize(p);
    spk::crop_and_rescale(src.data(), w, h, cr, &out->rgb, width, height,
                          pixel_size_um);
}

inline bool cancellation_requested(spk_cancel_check cancel,
                                   void* cancel_user_data) noexcept {
    return spk::parallel_cancellation_poll(cancel, cancel_user_data);
}

bool valid_diffusion_family(int32_t family) noexcept {
    return family >= SPK_DIFFUSION_DEFAULT &&
           family <= SPK_DIFFUSION_CINEBLOOM;
}

bool supported_input_color_space(const char* value) noexcept {
    return value == nullptr || value[0] == '\0' ||
           std::strcmp(value, "ProPhoto RGB") == 0;
}

bool valid_rgb_shape(const spk_image* image) noexcept {
    if (!image || !image->data || image->width <= 0 || image->height <= 0) {
        return false;
    }
    const auto pixels = static_cast<std::uint64_t>(image->width) *
                        static_cast<std::uint64_t>(image->height);
    return pixels <= static_cast<std::uint64_t>(
                         std::numeric_limits<int>::max()) &&
           pixels <= static_cast<std::uint64_t>(
                          std::numeric_limits<std::size_t>::max() / 3u);
}

// Validate every caller-controlled geometry value before crop_resize performs
// floating-point-to-integer casts or allocates a derived frame. The crop math
// mirrors crop_image's NumPy round/slice semantics closely enough to prove that
// both intermediate and final extents are non-empty and representable. The
// final byte cap is the same one enforced by the direct-ByteBuffer JNI seam.
bool valid_preprocess_geometry(const spk_image* image,
                               const spk_params* params) noexcept {
    if (!valid_rgb_shape(image) || !params) return false;

    const float raw_factor = params->upscale_factor;
    if (!std::isfinite(raw_factor) || raw_factor < 0.0f) return false;
    const double factor = raw_factor == 0.0f
                              ? 1.0
                              : static_cast<double>(raw_factor);

    std::int64_t crop_w = image->width;
    std::int64_t crop_h = image->height;
    if (params->crop != 0) {
        for (int axis = 0; axis < 2; ++axis) {
            const float center = params->crop_center[axis];
            const float size = params->crop_size[axis];
            if (!std::isfinite(center) || center < 0.0f || center > 1.0f ||
                !std::isfinite(size) || size <= 0.0f || size > 1.0f) {
                return false;
            }
        }

        const double h = static_cast<double>(image->height);
        const double w = static_cast<double>(image->width);
        const double longest = std::max(h, w);
        // center/size are (x,y); crop_image flips them to (row,column).
        const std::int64_t center_row = static_cast<std::int64_t>(
            std::nearbyint(h * params->crop_center[1]));
        const std::int64_t center_col = static_cast<std::int64_t>(
            std::nearbyint(w * params->crop_center[0]));
        const std::int64_t size_rows = static_cast<std::int64_t>(
            std::nearbyint(longest * params->crop_size[1]));
        const std::int64_t size_cols = static_cast<std::int64_t>(
            std::nearbyint(longest * params->crop_size[0]));
        if (size_rows <= 0 || size_cols <= 0) return false;

        std::int64_t row0 = static_cast<std::int64_t>(
            std::nearbyint(static_cast<double>(center_row) - size_rows / 2.0));
        std::int64_t col0 = static_cast<std::int64_t>(
            std::nearbyint(static_cast<double>(center_col) - size_cols / 2.0));
        if (row0 < 0) row0 = 0;
        if (col0 < 0) col0 = 0;
        if (row0 + size_rows > image->height) row0 = image->height - size_rows;
        if (col0 + size_cols > image->width) col0 = image->width - size_cols;

        const auto slice_extent = [](std::int64_t start, std::int64_t size,
                                     std::int64_t dimension) noexcept {
            std::int64_t stop = start + size;
            if (start < 0) start += dimension;
            if (stop < 0) stop += dimension;
            start = std::max<std::int64_t>(0, std::min(start, dimension));
            stop = std::max<std::int64_t>(0, std::min(stop, dimension));
            return stop > start ? stop - start : std::int64_t{0};
        };
        crop_h = slice_extent(row0, size_rows, image->height);
        crop_w = slice_extent(col0, size_cols, image->width);
        if (crop_w <= 0 || crop_h <= 0) return false;
    }

    const double rounded_w = std::nearbyint(factor * static_cast<double>(crop_w));
    const double rounded_h = std::nearbyint(factor * static_cast<double>(crop_h));
    if (!std::isfinite(rounded_w) || !std::isfinite(rounded_h) ||
        rounded_w > static_cast<double>(std::numeric_limits<int>::max()) ||
        rounded_h > static_cast<double>(std::numeric_limits<int>::max())) {
        return false;
    }
    const std::uint64_t out_w = static_cast<std::uint64_t>(
        std::max(1.0, rounded_w));
    const std::uint64_t out_h = static_cast<std::uint64_t>(
        std::max(1.0, rounded_h));
    if (out_h != 0 && out_w > std::numeric_limits<std::uint64_t>::max() / out_h) {
        return false;
    }
    const std::uint64_t pixels = out_w * out_h;
    constexpr std::uint64_t kRgbF32BytesPerPixel = 3u * sizeof(float);
    return pixels <= static_cast<std::uint64_t>(
                         std::numeric_limits<int>::max()) &&
           pixels <= static_cast<std::uint64_t>(INT32_MAX) /
                         kRgbF32BytesPerPixel &&
           pixels <= static_cast<std::uint64_t>(
                         std::numeric_limits<std::size_t>::max() / 3u);
}

// Run the scan_film pipeline, producing display RGB plus the intermediate taps.
// `tap_*` pointers, when non-null, receive the corresponding intermediate.
spk_status run_scan_film(spk_engine* eng, const spk_image* in, const spk_params* p,
                         std::vector<float>* final_rgb,
                         std::vector<float>* tap_log_raw,
                         std::vector<float>* tap_density_cmy,
                         int* out_w = nullptr, int* out_h = nullptr,
                         spk_cancel_check cancel = nullptr,
                         void* cancel_user_data = nullptr) try {
    if (!eng || !p || !valid_preprocess_geometry(in, p)) {
        return SPK_ERR_BAD_ARGS;
    }
    if (!p->film_profile) return SPK_ERR_BAD_ARGS;
    if (!supported_input_color_space(p->input_color_space) ||
        !valid_diffusion_family(p->camera_diffusion_family) ||
        !valid_diffusion_family(p->enlarger_diffusion_family)) {
        return SPK_ERR_BAD_ARGS;
    }
    spk::ParallelCancellationScope cancellation_scope(cancel, cancel_user_data);
    if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;

    // 0) Geometry preprocess (pipeline._preprocess -> crop_and_rescale). Runs
    //    BEFORE filming, mirroring Python ordering. Default params (crop off,
    //    upscale 1.0) leave the image, geometry and pixel_size_um unchanged.
    //    One-shot renders with no-op geometry come back in the DIRECT form
    //    (filming reads the caller's float32 frame; see PreprocessedInput).
    PreprocessedInput pin;
    int width = 0, height = 0;
    double resize_pixel_size_um = 0.0;
    { spk::ScopedStage _t(spk::STG_PREPROCESS);
      preprocess_geometry(in, p, &pin, &width, &height, &resize_pixel_size_um); }
    if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;
    const int npix = width * height;
    if (out_w) *out_w = width;
    if (out_h) *out_h = height;

    // 1) Load the film profile.
    spk::Profile film;
    try {
        film = load_engine_profile(eng, p->film_profile);
    } catch (const std::bad_alloc&) {
        return SPK_ERR_OOM;
    } catch (const ProfileAssetNotFound&) {
        return SPK_ERR_PROFILE_NOT_FOUND;
    } catch (const ProfileInvalid& e) {
        set_last_error_message(e.what());
        return SPK_ERR_PROFILE_INVALID;
    }
    if (film.log_sensitivity.empty() || film.log_exposure.empty() ||
        film.window_params.size() < 4) {
        return SPK_ERR_INTERNAL;  // profile lacks filming fields
    }
    if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;

    // 2) Digested filming params (auto-exposure off; stochastic/grain off).
    //    Spatial effects are PER-EFFECT gated, exactly like the oracle: absent
    //    the deactivate_spatial_effects debug switch (no C-API equivalent —
    //    callers express it by zeroing the per-effect fields), each effect runs
    //    off its own params and a ZERO value is inert (params_builder.py +
    //    per-effect self-gates, oracle c1d0e44). halation_active gates ONLY the
    //    halation/scatter block, mirroring oracle HalationParams.active.
    const bool halation_on = (p->halation_active != 0);
    const bool grain = (p->grain_active != 0);
    spk::FilmingParams fparams = spk::digest_filming_params(
        film.is_negative(), /*spatial_effects=*/true,
        !film.stock.empty() ? film.stock.c_str() : p->film_profile);
    fparams.exposure_compensation_ev = p->exposure_compensation_ev;
    const float g = p->density_curve_gamma != 0.0f ? p->density_curve_gamma : 1.0f;
    fparams.density_curve_gamma[0] = g;
    fparams.density_curve_gamma[1] = g;
    fparams.density_curve_gamma[2] = g;
    // DIR-coupler user params (amount / inhibition / diffusion); the per-channel
    // gamma matrices stay film-specific (baked by the digest). diffusion_size_um
    // <= 0 self-gates to the pointwise path.
    apply_user_dir_couplers(fparams.dir_couplers, p);
    // Camera optical diffusion filter — self-gated on camera_diffusion_active
    // (plus strength/scale inside apply_diffusion_filter_um), matching the
    // oracle's gate on diffusion_filter.active alone.
    apply_user_diffusion_filter(fparams.diffusion_filter, p, /*is_camera=*/true);
    // Camera lens blur (camera.lens_blur_um) — applied in expose() between the
    // diffusion filter and halation; self-gated on lens_blur_um > 0. Default
    // 0.0 µm => strict no-op, so default params stay bit-exact.
    fparams.lens_blur_um = static_cast<double>(p->lens_blur_um);
    // pixel_size_um drives the spatial kernels and the grain blur. It comes from
    // the resize service (film_format_mm*1000/max(orig h,w), then /=
    // upscale_factor), computed in preprocess_geometry above. Inert while every
    // spatial effect self-gates off.
    fparams.pixel_size_um = resize_pixel_size_um;
    if (grain) {
        // grain_active && stochastic effects on -> AgX particle grain. The
        // density_max_curves are filled inside develop() from the film's
        // normalized density curves.
        spk::digest_grain_params(fparams);
        apply_user_grain(fparams.grain, p);
    }
    if (halation_on) {
        spk::digest_halation_params(fparams, film.use.c_str(),
                                    film.antihalation.c_str(), true);
        // halation user params (everything except the preset-baked sigma/strength).
        apply_user_halation(fparams.halation, p);
    }

    // Highlight boost is NOT a spatial effect: the oracle applies boost_highlights in
    // filming.expose regardless of deactivate_spatial_effects (params_builder.py only
    // zeroes the scatter/halation sigmas, never boost_ev), and apply_highlight_boost
    // gates on boost_ev > 0. Thread the three boost params into fparams.halation
    // UNCONDITIONALLY so the boost is reachable on the spatial-OFF path too (where the
    // block above is skipped). Idempotent when spatial is ON. boost_ev defaults to 0
    // (schema/UI) -> a strict no-op, so default goldens stay bit-exact.
    fparams.halation.boost_ev = p->halation_boost_ev;
    fparams.halation.boost_range = p->halation_boost_range;
    fparams.halation.protect_ev = p->halation_protect_ev;

    // 3) Build (or reuse the engine-cached) Hanatos2025 filming tc_lut (D55
    //    reference illuminant). Cached by film id — byte-identical to rebuilding
    //    (see engine_tc_lut / the spk_engine cache note).
    const spk::NdArray* tc_lut_ptr = nullptr;
    try {
        spk::ScopedStage _t(spk::STG_TC_LUT);  // cold-start heavy (#152)
        tc_lut_ptr = &engine_tc_lut(eng, p->film_profile, film, p->spectral_gaussian_blur,
                                    p->apply_hanatos_window != 0,
                                    p->apply_hanatos_surface != 0,
                                    p->camera_filter_uv, p->camera_filter_ir,
                                    static_cast<spk::InputGamutCompress>(
                                        p->input_gamut_compress));
    } catch (const std::exception&) {
        return SPK_ERR_ASSET_IO;
    }
    const spk::NdArray& tc_lut = *tc_lut_ptr;
    if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;

    // 4) expose(): the image runs as float64 (ProPhoto linear). `rgb` was built
    //    by preprocess_geometry above (crop/rescale applied, float64), matching
    //    the Python pipeline.
    // Scanner BLACK/WHITE correction (scan_film route). Active only when a
    // correction is enabled AND the film is POSITIVE (color_reference.py returns a
    // no-op for negative film + the scan_film negative route). For positive film we
    // measure the reference Y values from the film density curves, build the shared
    // affine (m, q, midgray_corrected), and apply the FILMING exposure correction
    // here + the XYZ correction in scan() below. For negative film everything stays
    // a strict no-op so the default scan goldens are bit-exact.
    spk::ColorCorrection bw_corr;  // inactive by default
    const bool bw_on = (p->scanner_white_correction != 0) ||
                       (p->scanner_black_correction != 0);
    if (bw_on && film.is_positive()) {
        double y_black, y_white;
        spk::measure_scanfilm_references(film, &y_black, &y_white);
        bw_corr = spk::build_color_correction(
            y_black, y_white,
            spk::remove_srgb_cctf(static_cast<double>(p->scanner_black_level)),
            spk::remove_srgb_cctf(static_cast<double>(p->scanner_white_level)),
            p->scanner_black_correction != 0, p->scanner_white_correction != 0);
        fparams.bw_exposure_correction =
            spk::exposure_correction_factor(film, bw_corr, /*filming_positive=*/true);
    }

    // Scan-route film_density_cmy memo (the scan-route twin of the print memo
    // below — see the spk_engine::film_memo comment). With grain off,
    // expose()+develop() is a PURE deterministic function of the folded inputs;
    // the key includes every spatial shape param (Option A) plus this route's
    // bw_exposure_correction (applied to raw inside expose), so spatial-ON
    // renders memoize and a downstream-only edit (scanner/output/tone-curve)
    // skips filming entirely. Taps + grain bypass.
    // Allocated when a path needs it (memo hit copy-assigns; the miss path
    // resizes right before develop, AFTER expose's transients are gone —
    // EXPORT_FASTPATH item 4 keeps it out of the filming peak).
    std::vector<float> density_cmy;
    const size_t out_elems = static_cast<size_t>(npix) * 3;
    const bool scan_tap_bypass =
        (tap_log_raw != nullptr) || (tap_density_cmy != nullptr);
    // Also skipped for one-shot renders (disable_buffer_memos, EXPORT_FASTPATH
    // item 2): no key hash, no lookup, no store — the warm slot stays intact.
    const bool use_scan_film_cache =
        !scan_tap_bypass && !grain && p->disable_buffer_memos == 0;

    bool scan_film_cache_hit = false;
    // Computed ONCE per render (EXPORT_FASTPATH item 2): the key hashes the
    // whole float64 rgb buffer, so the old check-then-store recompute paid
    // that full-buffer hash twice on every miss. Only reachable in the
    // materialized form: use_scan_film_cache requires disable_buffer_memos==0
    // and the direct form requires it != 0, so pin.rgb is always valid here.
    uint64_t scan_film_key = 0;
    if (use_scan_film_cache) {
        scan_film_key = compute_film_cache_key(
            pin.rgb, width, height, in->color_space, p, resize_pixel_size_um,
            fparams.bw_exposure_correction);
        std::lock_guard<std::mutex> g(eng->film_cache_mutex);
        auto& slot = eng->film_memo[spk_engine::kMemoScan];
        if (slot.valid && slot.key == scan_film_key &&
            slot.entry.width == width && slot.entry.height == height &&
            slot.entry.film_density_cmy.size() == out_elems) {
            // HIT: copy the cached buffer out BY VALUE while holding the lock.
            density_cmy = slot.entry.film_density_cmy;
            ++slot.hits;
            scan_film_cache_hit = true;
        }
        // MISS path is handled after unlocking (we don't compute under the lock).
    }
    if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;

    if (!scan_film_cache_hit) {
        std::vector<float> log_raw(out_elems);
        if (pin.direct) {
            spk::expose_f32_gain(in->data, pin.gain, width, height, fparams,
                                 tc_lut, log_raw.data());
        } else {
            spk::expose(pin.rgb.data(), width, height, fparams, tc_lut,
                        log_raw.data());
        }
        if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;
        if (tap_log_raw) *tap_log_raw = log_raw;

        // The float64 image is dead past expose — at 12 MP this releases
        // ~288 MB before develop/scan run (EXPORT_FASTPATH item 4). The memo
        // key was computed above; taps never read it.
        { std::vector<double>().swap(pin.rgb); }

        // 5) develop(): log_raw -> density_cmy (+ DIR couplers, spatial diffusion if on).
        density_cmy.resize(out_elems);
        spk::develop(log_raw.data(), width, height, film, fparams, density_cmy.data());
        if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;
        if (use_scan_film_cache) {
            std::lock_guard<std::mutex> g(eng->film_cache_mutex);
            auto& slot = eng->film_memo[spk_engine::kMemoScan];
            slot.entry.width = width;
            slot.entry.height = height;
            slot.entry.film_density_cmy = density_cmy;
            slot.key = scan_film_key;
            slot.valid = true;
            ++slot.misses;
        }
    } else {
        { std::vector<double>().swap(pin.rgb); }
    }
    if (tap_density_cmy) *tap_density_cmy = density_cmy;

    if (!final_rgb) return SPK_OK;  // caller only wanted an earlier tap
    if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;

    // 6) scan(): density_cmy -> display RGB (output_color_space, CCTF per params).
    spk::ScanningParams sparams;
    sparams.scan_film = true;
    // GPU preview fast-path (#146): set only when spk_simulate_preview latched
    // allow_gpu_scan; scan() re-gates on frame eligibility + the self-check.
    sparams.allow_gpu = (p->allow_gpu_scan != 0);
    sparams.output_color_space = p->output_color_space;
    sparams.output_cctf_encoding = (p->output_cctf_encoding != 0);
    // OPT-IN output gamut compression (scan_film route). Default kLegacyClip (0) keeps
    // scan()'s existing final clip, so the default goldens stay byte-identical; the
    // knee stays at the ScanningParams oracle production default (0,1,6).
    sparams.output_gamut_compress =
        static_cast<spk::OutputGamutCompress>(p->output_gamut_compress);
    // scanner.unsharp_mask = (sigma, amount) + scanner lens_blur (pixels). Each
    // self-gates inside scan() (sigma > 0, amount > 0, blur > 0), exactly the
    // oracle's gates; the deactivate_spatial_effects debug switch is expressed by
    // passing zeros. Threaded unconditionally so the scanner sharpening no longer
    // rides on halation_active.
    sparams.unsharp_sigma = p->scanner_unsharp[0];
    sparams.unsharp_amount = p->scanner_unsharp[1];
    sparams.lens_blur = static_cast<double>(p->scanner_lens_blur);
    // OPT-IN scanner 3D-LUT acceleration (settings.use_scanner_lut, default 0).
    // When off (the default + parity-gate path) scan() never constructs the LUT and
    // is byte-identical to the direct spectral evaluation. When on, scan() routes
    // density_cmy -> log_xyz through the PCHIP 3D LUT at settings.lut_resolution
    // (clamped), mirroring scanning.py::_density_to_rgb(use_lut=use_scanner_lut).
    if (p->use_scanner_lut != 0) {
        sparams.use_lut = true;
        sparams.lut_resolution = p->lut_resolution;
        sparams.lut_cache = &eng->lut_cache;  // memo the build (PERF, byte-identical)
    }
    sparams.tone_curve = build_tone_curve_set(p);
    // Scanner BLACK/WHITE XYZ correction (scan_film route): apply the shared affine
    // (m, q) per pixel in scan(). Inactive (strict no-op) for negative film.
    if (bw_corr.active) {
        sparams.bw_xyz_correction = true;
        sparams.bw_xyz_m = bw_corr.m;
        sparams.bw_xyz_q = bw_corr.q;
    }

    final_rgb->assign(static_cast<size_t>(npix) * 3, 0.0f);
    spk::scan(film, sparams, density_cmy.data(), width, height, final_rgb->data());
    if (cancellation_requested(cancel, cancel_user_data)) {
        final_rgb->clear();
        return SPK_ERR_CANCELLED;
    }
    return SPK_OK;
} catch (const spk::ParallelCancelled&) {
    if (final_rgb) final_rgb->clear();
    if (tap_log_raw) tap_log_raw->clear();
    if (tap_density_cmy) tap_density_cmy->clear();
    return SPK_ERR_CANCELLED;
}

// Run the negative -> print -> scan route, producing display RGB plus the
// intermediate taps. `tap_*` pointers, when non-null, receive the corresponding
// intermediate. Mirrors pipeline._pipeline_print under the print_portra toggles.
spk_status run_print(spk_engine* eng, const spk_image* in, const spk_params* p,
                     std::vector<float>* final_rgb,
                     std::vector<float>* tap_log_raw,
                     std::vector<float>* tap_film_density_cmy,
                     std::vector<float>* tap_print_density_cmy,
                     int* out_w = nullptr, int* out_h = nullptr,
                     spk_cancel_check cancel = nullptr,
                     void* cancel_user_data = nullptr) try {
    if (!eng || !p || !valid_preprocess_geometry(in, p)) {
        return SPK_ERR_BAD_ARGS;
    }
    if (!p->film_profile || !p->print_profile) return SPK_ERR_BAD_ARGS;
    if (!supported_input_color_space(p->input_color_space) ||
        !valid_diffusion_family(p->camera_diffusion_family) ||
        !valid_diffusion_family(p->enlarger_diffusion_family)) {
        return SPK_ERR_BAD_ARGS;
    }
    spk::ParallelCancellationScope cancellation_scope(cancel, cancel_user_data);
    if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;

    // 0) Geometry preprocess (crop_and_rescale) BEFORE filming, mirroring the
    //    Python pipeline._preprocess. Default params -> passthrough. The print
    //    route runs spatial/grain off, so pixel_size_um is not consumed here;
    //    only the geometry (width/height) can change. One-shot renders with
    //    no-op geometry come back in the DIRECT form (filming reads the
    //    caller's float32 frame; see PreprocessedInput).
    PreprocessedInput pin;
    int width = 0, height = 0;
    double resize_pixel_size_um = 0.0;
    { spk::ScopedStage _t(spk::STG_PREPROCESS);
      preprocess_geometry(in, p, &pin, &width, &height, &resize_pixel_size_um); }
    if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;
    const int npix = width * height;
    if (out_w) *out_w = width;
    if (out_h) *out_h = height;

    // 1) Load film + print profiles.
    spk::Profile film, prnt;
    try {
        film = load_engine_profile(eng, p->film_profile);
        prnt = load_engine_profile(eng, p->print_profile);
    } catch (const std::bad_alloc&) {
        return SPK_ERR_OOM;
    } catch (const ProfileAssetNotFound&) {
        return SPK_ERR_PROFILE_NOT_FOUND;
    } catch (const ProfileInvalid& e) {
        set_last_error_message(e.what());
        return SPK_ERR_PROFILE_INVALID;
    }
    if (film.log_sensitivity.empty() || film.log_exposure.empty() ||
        film.window_params.size() < 4) {
        return SPK_ERR_INTERNAL;  // film profile lacks filming fields
    }
    if (prnt.log_sensitivity.empty() || prnt.log_exposure.empty() ||
        prnt.density_curves.empty()) {
        return SPK_ERR_INTERNAL;  // print profile lacks printing fields
    }
    if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;

    // 2) Build (or reuse the engine-cached) Hanatos2025 filming tc_lut (D55
    //    reference illuminant). Needed both by the filming expose and by the
    //    native midgray digest below. Cached by film id — byte-identical to
    //    rebuilding (see engine_tc_lut / the spk_engine cache note).
    const spk::NdArray* tc_lut_ptr = nullptr;
    try {
        spk::ScopedStage _t(spk::STG_TC_LUT);  // cold-start heavy (#152)
        tc_lut_ptr = &engine_tc_lut(eng, p->film_profile, film, p->spectral_gaussian_blur,
                                    p->apply_hanatos_window != 0,
                                    p->apply_hanatos_surface != 0,
                                    p->camera_filter_uv, p->camera_filter_ir,
                                    static_cast<spk::InputGamutCompress>(
                                        p->input_gamut_compress));
    } catch (const std::exception&) {
        return SPK_ERR_ASSET_IO;
    }
    const spk::NdArray& tc_lut = *tc_lut_ptr;
    if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;

    // 3) Native print digest for ANY (film, paper) pair:
    //    (a) neutral dichroic CC resolved from neutral_print_filters.json
    //        ([print_stock][illuminant][film_stock]); missing triples fall back
    //        to the schema defaults {0,0,0}, mirroring params_builder.py.
    //    (b) midgray exposure factor computed natively from the filming midgray
    //        balance + the print sensitivity/filtered illuminant.
    const char* illuminant = p->enlarger_illuminant;
    if (!illuminant || illuminant[0] == '\0') illuminant = "TH-KG3";
    const double* enl = spk::enlarger_illuminant(illuminant);
    if (!enl) {
        set_last_error_message("unsupported enlarger illuminant");
        return SPK_ERR_BAD_ARGS;
    }
    const std::string film_stock  = !film.stock.empty() ? film.stock : p->film_profile;
    const std::string print_stock = !prnt.stock.empty() ? prnt.stock : p->print_profile;
    double neutral_cc[3];
    if (p->neutral_print_filters_from_database) {
        // Read neutral_print_filters.json via the asset abstraction (FS or AAsset),
        // then resolve from the in-memory bytes. A missing/unreadable asset yields
        // defaults {0,0,0}, mirroring the Python FileNotFoundError branch.
        spk::ScopedStage _t(spk::STG_PRINT_DIGEST);
        std::vector<char> nf;
        if (spk_read_asset(eng, kNeutralFiltersRel, nf)) {
            spk::resolve_neutral_cc_string(std::string(nf.data(), nf.size()),
                                           print_stock, illuminant, film_stock,
                                           neutral_cc);
        } else {
            neutral_cc[0] = neutral_cc[1] = neutral_cc[2] = 0.0;
        }
    } else {
        // Use the schema neutral CC values directly (filter_enlarger_source uses
        // [c_filter_neutral, m_filter_neutral, y_filter_neutral] in CMY order).
        neutral_cc[0] = p->c_filter_neutral;
        neutral_cc[1] = p->m_filter_neutral;
        neutral_cc[2] = p->y_filter_neutral;
    }
    // Preserve the UN-SHIFTED neutral CC for the preflash filtered illuminant,
    // which applies its OWN m/y shifts to the neutral values (NOT the image
    // exposure's m/y shifts) per filter_enlarger_source.preflash_filtered_illuminant.
    const double base_neutral_cc[3] = {neutral_cc[0], neutral_cc[1], neutral_cc[2]};
    // filtered_illuminant CC = [c_neutral, m_neutral + m_shift, y_neutral + y_shift]
    // (filter_enlarger_source.filtered_illuminant). Shifts default to 0, so the
    // resolved neutral CC is used unchanged under the parity defaults.
    neutral_cc[1] += static_cast<double>(p->m_filter_shift);
    neutral_cc[2] += static_cast<double>(p->y_filter_shift);

    // Print density-curve gamma is independent of the film gamma.
    const float pg = p->print_density_curve_gamma != 0.0f ? p->print_density_curve_gamma : 1.0f;
    // Build the filtered illuminant first (color_enlarger with neutral CC), then
    // use it for the native midgray factor.
    spk::PrintingParams pparams = spk::digest_printing_params(
        neutral_cc, enl, /*exposure_factor_midgray=*/1.0, pg);
    { spk::ScopedStage _t(spk::STG_PRINT_DIGEST);
      pparams.exposure_factor_midgray = spk::compute_midgray_exposure_factor(
          film, prnt, tc_lut, pparams.filtered_illuminant, pg,
          static_cast<double>(p->exposure_compensation_ev),
          p->normalize_print_exposure != 0, p->print_exposure_compensation != 0); }
    // enlarger.print_exposure (default 1.0) multiplies the print exposure.
    pparams.print_exposure = p->print_exposure;
    // Same GPU latch scanning uses (INTERNAL — never the raw user flag), so one
    // toggle governs both offloads. print_expose re-gates per frame and falls
    // back to the exact CPU integral on any failure.
    pparams.allow_gpu = (p->allow_gpu_scan != 0);

    // OPT-IN s023 print density-curve morph (print_render.density_curves_morph).
    // Default-off -> print_develop uses the stored density_curves table (the
    // parity-gate path). The morph is downstream of the cached film_density_cmy,
    // so it needs no film-cache-key fold (the cache memoises only film density).
    pparams.morph.active = p->print_morph_active != 0;
    pparams.morph.gamma_factor = p->print_morph_gamma_factor;
    pparams.morph.gamma_factor_fast = p->print_morph_gamma_factor_fast;
    pparams.morph.gamma_factor_slow = p->print_morph_gamma_factor_slow;
    pparams.morph.gamma_factor_red = p->print_morph_gamma_factor_red;
    pparams.morph.gamma_factor_green = p->print_morph_gamma_factor_green;
    pparams.morph.gamma_factor_blue = p->print_morph_gamma_factor_blue;
    pparams.morph.developer_exhaustion = p->print_morph_developer_exhaustion;

    // Enlarger PREFLASH (printing.py::_compute_raw_preflash via
    // filter_enlarger_source.preflash_filtered_illuminant). The preflash flashes
    // the paper through the enlarger with its OWN filter shifts off the neutral CC:
    //   preflash CC = [c_neutral, m_neutral + preflash_m_filter_shift,
    //                  y_neutral + preflash_y_filter_shift]
    // (the image-exposure m/y shifts are NOT applied to the preflash). print_expose
    // then adds raw_preflash * preflash_exposure (a constant per-channel 3-vector,
    // = sum_l 10^-base_density[l] * preflash_illuminant[l] * sens[l,k]) to the print
    // raw, after the midgray factor. preflash_exposure default 0.0 => print_expose's
    // `if preflash_exposure > 0` guard makes it a STRICT no-op.
    pparams.preflash_exposure = static_cast<double>(p->preflash_exposure);
    if (pparams.preflash_exposure > 0.0) {
        double preflash_cc[3] = {
            base_neutral_cc[0],
            base_neutral_cc[1] + static_cast<double>(p->preflash_m_filter_shift),
            base_neutral_cc[2] + static_cast<double>(p->preflash_y_filter_shift),
        };
        spk::color_enlarger(enl, preflash_cc, pparams.preflash_illuminant);
    }
    // OPT-IN enlarger 3D-LUT acceleration (settings.use_enlarger_lut, default 0).
    // When off (the default + parity-gate path) print_expose never builds the LUT
    // and is byte-identical to the direct spectral integral. When on, print_expose
    // routes film density_cmy -> print log_raw through the PCHIP 3D LUT at
    // settings.lut_resolution (clamped), mirroring printing.py::expose
    // (spectral_compute_enlarger, use_lut=use_enlarger_lut). The print-route final
    // scan() still honours use_scanner_lut independently below.
    if (p->use_enlarger_lut != 0) {
        pparams.use_enlarger_lut = true;
        pparams.lut_resolution = p->lut_resolution;
        pparams.lut_cache = &eng->lut_cache;  // memo the build (PERF, byte-identical)
        pparams.grain_density_min[0] = static_cast<double>(p->grain_density_min[0]);
        pparams.grain_density_min[1] = static_cast<double>(p->grain_density_min[1]);
        pparams.grain_density_min[2] = static_cast<double>(p->grain_density_min[2]);
    }
    // Halation/scatter gate for the print route's filming step. halation_active
    // gates ONLY halation (its UI meaning), mirroring run_scan_film; every other
    // spatial effect self-gates on its own params (zero = inert).
    const bool print_halation_on = (p->halation_active != 0);
    // Stochastic branch toggle for the print route (mirrors the negative scan:
    // deactivate_stochastic_effects=False enables grain + viewing glare).
    const bool print_stochastic = (p->grain_active != 0);
    // Enlarger optical diffusion filter, applied in print_expose on the float64
    // print irradiance before the final log10. Self-gated on
    // enlarger_diffusion_active (the oracle's only gate — deactivate_spatial_effects
    // is expressed by zeroing .active). Inactive by default -> strict no-op.
    // pixel_size_um from the resize service drives the µm->pixel conversion.
    apply_user_diffusion_filter(pparams.diffusion_filter, p, /*is_camera=*/false);
    pparams.pixel_size_um = resize_pixel_size_um;

    // Scanner BLACK/WHITE correction (print route). Active only when a correction is
    // enabled AND the print paper is NEGATIVE (color_reference.py's
    // black_white_printing_exposure_correction computes only for print.type ==
    // 'negative'; the xyz correction is active on the whole print route). The two
    // reference Y values come from develop_simple of the print reference log_raw
    // vectors, which are _film_cmy_to_print_log_raw of the film black/white
    // reference CMY densities (cmy_film_black = -grain.density_min, cmy_film_white =
    // nanmax(film.density_curves)) through the LIVE enlarger params (pparams must be
    // fully built — filtered illuminant, midgray factor, preflash — by here). We
    // then set the PRINTING exposure correction (pparams.bw_exposure_correction,
    // already plumbed into print_expose) + the XYZ correction in the print scan()
    // below. Default-off / non-negative-paper => strict no-op, so the print goldens
    // stay bit-exact.
    spk::ColorCorrection bw_corr;  // inactive by default
    const bool bw_on = (p->scanner_white_correction != 0) ||
                       (p->scanner_black_correction != 0);
    if (bw_on && prnt.is_negative()) {
        // cmy_film_black = -grain.density_min; cmy_film_white = nanmax(film curves).
        double cmy_film_black[3] = {
            -static_cast<double>(p->grain_density_min[0]),
            -static_cast<double>(p->grain_density_min[1]),
            -static_cast<double>(p->grain_density_min[2])};
        double cmy_film_white[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (int n = 0; n < film.n_density_pts; ++n) {
            const float* dc =
                film.density_curves.data() + static_cast<size_t>(n) * 3;
            for (int k = 0; k < 3; ++k) {
                double v = static_cast<double>(dc[k]);
                if (!std::isnan(v) && v > cmy_film_white[k]) cmy_film_white[k] = v;
            }
        }
        double log_raw_black[3], log_raw_white[3];
        spk::print_reference_log_raw(film, prnt, pparams, cmy_film_black,
                                     log_raw_black);
        spk::print_reference_log_raw(film, prnt, pparams, cmy_film_white,
                                     log_raw_white);
        double y_black, y_white;
        spk::measure_print_references(prnt, log_raw_black, log_raw_white, pg,
                                      &y_black, &y_white);
        bw_corr = spk::build_color_correction(
            y_black, y_white,
            spk::remove_srgb_cctf(static_cast<double>(p->scanner_black_level)),
            spk::remove_srgb_cctf(static_cast<double>(p->scanner_white_level)),
            p->scanner_black_correction != 0, p->scanner_white_correction != 0);
        pparams.bw_exposure_correction = spk::exposure_correction_factor(
            prnt, bw_corr, /*filming_positive=*/false);
    }

    // 4) Filming stage (rgb -> film density_cmy), reusing the bit-exact port.
    //    The print route now runs the SAME per-effect-gated filming as the scan
    //    route — the oracle runs one FilmingStage on both routes, so the
    //    negative carries its halation / scatter / DIR diffusion / lens blur /
    //    grain into the print. Each effect self-gates on its own params.
    spk::FilmingParams fparams = spk::digest_filming_params(
        film.is_negative(), /*spatial_effects=*/true,
        !film.stock.empty() ? film.stock.c_str() : p->film_profile);
    fparams.exposure_compensation_ev = p->exposure_compensation_ev;
    const float fg = p->density_curve_gamma != 0.0f ? p->density_curve_gamma : 1.0f;
    fparams.density_curve_gamma[0] = fg;
    fparams.density_curve_gamma[1] = fg;
    fparams.density_curve_gamma[2] = fg;
    apply_user_dir_couplers(fparams.dir_couplers, p);
    apply_user_diffusion_filter(fparams.diffusion_filter, p, /*is_camera=*/true);
    fparams.lens_blur_um = static_cast<double>(p->lens_blur_um);
    fparams.pixel_size_um = resize_pixel_size_um;
    if (print_stochastic) {
        // grain_active -> AgX particle grain inside develop(), exactly as the
        // scan route wires it. Deterministic seed; stays serial.
        spk::digest_grain_params(fparams);
        apply_user_grain(fparams.grain, p);
    }
    if (print_halation_on) {
        spk::digest_halation_params(fparams, film.use.c_str(),
                                    film.antihalation.c_str(), true);
        // halation user params (everything except the preset-baked sigma/strength).
        apply_user_halation(fparams.halation, p);
    }
    // Highlight boost is NOT a spatial effect (it runs in filming.expose regardless of
    // deactivate_spatial_effects). The print route's negative-filming runs spatial-OFF,
    // so thread the boost params in directly; apply_highlight_boost gates on
    // boost_ev > 0. Folded into compute_film_cache_key, so a boost edit busts the
    // film-density memo. boost_ev defaults to 0 -> a strict no-op.
    fparams.halation.boost_ev = p->halation_boost_ev;
    fparams.halation.boost_range = p->halation_boost_range;
    fparams.halation.protect_ev = p->halation_protect_ev;

    // `pin` (the preprocessed input — materialized float64 or the direct
    // float32 form) was built by preprocess_geometry above.

    // Print-route film_density_cmy memo. With grain off, expose()+develop() is a
    // PURE deterministic function of (rgb, width, height, color_space, filming
    // params) — the deterministic spatial effects are fixed convolutions, and the
    // key folds every spatial shape param (Option A) so spatial-ON renders
    // memoize too. Everything downstream runs unchanged on every call, so the
    // cache is transparent and bit-exact.
    //
    // Cache is BYPASSED (always recompute) when:
    //   (a) a debug tap (log_raw / film_density_cmy) is requested — keeps the tap
    //       path byte-identical and avoids caching tap-only renders, OR
    //   (b) grain is on (stochastic), OR
    //   (c) the caller opted out for a one-shot render (disable_buffer_memos,
    //       EXPORT_FASTPATH item 2): no key hash, no lookup, no store — and the
    //       warm slot stays intact.
    // Allocated when a path needs it (memo hit copy-assigns; the miss path
    // resizes right before develop, AFTER expose's transients are gone —
    // EXPORT_FASTPATH item 4 keeps it out of the filming peak).
    std::vector<float> film_density_cmy;
    const size_t out_elems = static_cast<size_t>(npix) * 3;
    const bool tap_bypass = (tap_log_raw != nullptr) || (tap_film_density_cmy != nullptr);
    const bool use_film_cache =
        !tap_bypass && !print_stochastic && p->disable_buffer_memos == 0;

    bool film_cache_hit = false;
    // Computed ONCE per render (EXPORT_FASTPATH item 2): the key hashes the
    // whole float64 rgb buffer, so the old check-then-store recompute paid
    // that full-buffer hash twice on every miss. Only reachable in the
    // materialized form: use_film_cache requires disable_buffer_memos==0 and
    // the direct form requires it != 0, so pin.rgb is always valid here.
    uint64_t film_key = 0;
    if (use_film_cache) {
        film_key = compute_film_cache_key(pin.rgb, width, height,
                                          in->color_space, p,
                                          resize_pixel_size_um,
                                          /*bw_exposure_correction=*/1.0);
        std::lock_guard<std::mutex> g(eng->film_cache_mutex);
        auto& slot = eng->film_memo[spk_engine::kMemoPrint];
        if (slot.valid && slot.key == film_key &&
            slot.entry.width == width && slot.entry.height == height &&
            slot.entry.film_density_cmy.size() == out_elems) {
            // HIT: copy the cached buffer out BY VALUE while holding the lock.
            film_density_cmy = slot.entry.film_density_cmy;
            ++slot.hits;
            film_cache_hit = true;
        }
        // MISS path is handled after unlocking (we don't compute under the lock).
    }
    if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;

    if (!film_cache_hit) {
        std::vector<float> film_log_raw(out_elems);
        if (pin.direct) {
            spk::expose_f32_gain(in->data, pin.gain, width, height, fparams,
                                 tc_lut, film_log_raw.data());
        } else {
            spk::expose(pin.rgb.data(), width, height, fparams, tc_lut,
                        film_log_raw.data());
        }
        if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;
        if (tap_log_raw) *tap_log_raw = film_log_raw;

        // The float64 image is dead past expose — at 12 MP this releases
        // ~288 MB before develop/printing/scan run (EXPORT_FASTPATH item 4).
        // The memo key was computed above; taps never read it.
        { std::vector<double>().swap(pin.rgb); }

        film_density_cmy.resize(out_elems);
        spk::develop(film_log_raw.data(), width, height, film, fparams,
                     film_density_cmy.data());
        if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;
        if (use_film_cache) {
            // Store {width, height, film_density_cmy} + key under the lock, count miss.
            std::lock_guard<std::mutex> g(eng->film_cache_mutex);
            auto& slot = eng->film_memo[spk_engine::kMemoPrint];
            slot.entry.width = width;
            slot.entry.height = height;
            slot.entry.film_density_cmy = film_density_cmy;
            slot.key = film_key;
            slot.valid = true;
            ++slot.misses;
        }
    } else {
        { std::vector<double>().swap(pin.rgb); }
    }
    if (tap_film_density_cmy) *tap_film_density_cmy = film_density_cmy;

    // 4) Printing stage (film density -> enlarger expose -> print develop),
    //    memoized on the film_density_cmy CONTENT + every printing input (see
    //    compute_print_density_key). An output-only edit (scanner/output space/
    //    tone curve/glare) therefore reruns scan() alone. Debug-tap renders
    //    bypass, keeping the tap path byte-identical to a plain recompute.
    std::vector<float> print_density_cmy;  // allocated by the path that fills it
    // Bypassed for tap renders AND for one-shot renders (disable_buffer_memos,
    // EXPORT_FASTPATH item 2 — the key hashes the whole float32 film buffer).
    const bool pd_bypass = tap_bypass || (tap_print_density_cmy != nullptr) ||
                           p->disable_buffer_memos != 0;
    bool pd_hit = false;
    // Computed ONCE per render (item 2): was recomputed on the store leg of
    // every miss, hashing the full film_density_cmy buffer twice.
    uint64_t pd_key = 0;
    if (!pd_bypass) {
        pd_key = compute_print_density_key(
            film_density_cmy, width, height, p, resize_pixel_size_um);
        std::lock_guard<std::mutex> g(eng->film_cache_mutex);
        auto& slot = eng->print_density_memo;
        if (slot.valid && slot.key == pd_key &&
            slot.entry.width == width && slot.entry.height == height &&
            slot.entry.film_density_cmy.size() == out_elems) {
            print_density_cmy = slot.entry.film_density_cmy;  // holds print density
            ++slot.hits;
            pd_hit = true;
        }
    }
    if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;
    if (!pd_hit) {
        std::vector<float> print_log_raw(out_elems);
        spk::print_expose(film, prnt, pparams, film_density_cmy.data(), width, height,
                          print_log_raw.data());
        if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;
        // film_density_cmy's last read was print_expose (the tap copy was taken
        // above; the memo store below uses pd_key only) — at 12 MP this
        // releases ~144 MB before print_develop/scan run (EXPORT_FASTPATH
        // item 4).
        { std::vector<float>().swap(film_density_cmy); }
        print_density_cmy.resize(out_elems);
        spk::print_develop(prnt, pparams, print_log_raw.data(), npix,
                           print_density_cmy.data());
        if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;
        if (!pd_bypass) {
            std::lock_guard<std::mutex> g(eng->film_cache_mutex);
            auto& slot = eng->print_density_memo;
            slot.entry.width = width;
            slot.entry.height = height;
            slot.entry.film_density_cmy = print_density_cmy;  // holds print density
            slot.key = pd_key;
            slot.valid = true;
            ++slot.misses;
        }
    } else {
        { std::vector<float>().swap(film_density_cmy); }
    }
    if (tap_print_density_cmy) *tap_print_density_cmy = print_density_cmy;

    if (!final_rgb) return SPK_OK;  // caller only wanted an earlier tap
    if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;

    // 5) Scan the print (D50 viewing illuminant, print profile's dyes).
    spk::ScanningParams sparams;
    sparams.scan_film = false;
    // GPU preview fast-path (#146): set only when spk_simulate_preview latched
    // allow_gpu_scan; scan() re-gates on frame eligibility + the self-check.
    sparams.allow_gpu = (p->allow_gpu_scan != 0);
    sparams.output_color_space = p->output_color_space;
    sparams.output_cctf_encoding = (p->output_cctf_encoding != 0);
    // OPT-IN output gamut compression (print route, same scan() position as scan_film).
    // Default kLegacyClip (0) keeps the existing clip so print goldens stay byte-identical;
    // the knee stays at the ScanningParams oracle production default (0,1,6).
    sparams.output_gamut_compress =
        static_cast<spk::OutputGamutCompress>(p->output_gamut_compress);
    // Viewing glare on the PRINT route (scanning.py applies glare = print_render.glare
    // here, in XYZ space, before XYZ->RGB). It is STOCHASTIC (per-pixel lognormal via
    // np.random.randn) so an active result is NOT bit-exact vs the oracle (exactly
    // like grain). The oracle's digest_params sets print_render.glare.active = False
    // under deactivate_stochastic_effects=True, i.e. glare is part of the STOCHASTIC
    // branch — the same branch grain belongs to. The print parity goldens were
    // generated with deactivate_stochastic_effects=True, so glare was OFF and the
    // default native print output (glare off) stays bit-exact.
    //
    // We therefore gate native glare on the stochastic branch (proxied by
    // grain_active, the engine's stochastic toggle, mirroring how the oracle ties
    // both grain and glare to deactivate_stochastic_effects) AND print_glare_active.
    // With the parity toggles (grain_active == 0) glare never runs. When a caller
    // turns the stochastic branch on AND enables print glare, the effect is applied
    // at the oracle position but held to a visual (not bit-exact) tolerance.
    if (print_stochastic && p->print_glare_active != 0) {
        sparams.glare_active = true;
        sparams.glare_percent = p->print_glare_percent;
        sparams.glare_roughness = p->print_glare_roughness;
        sparams.glare_blur = p->print_glare_blur;
    }
    // Scanner lens blur + unsharp on the print route (scanning.py runs the same
    // _apply_blur_and_unsharp on both routes). Each self-gates inside scan()
    // (sigma > 0, amount > 0, blur > 0) exactly like the oracle; the
    // deactivate_spatial_effects debug switch is expressed by passing zeros.
    sparams.lens_blur = static_cast<double>(p->scanner_lens_blur);
    sparams.unsharp_sigma = p->scanner_unsharp[0];
    sparams.unsharp_amount = p->scanner_unsharp[1];
    // OPT-IN scanner 3D-LUT acceleration on the print-scan route (same
    // settings.use_scanner_lut gate; scan_film == false so scan() picks the print
    // density-curve domain bounds). Default 0 => never constructed, print goldens
    // stay bit-exact.
    if (p->use_scanner_lut != 0) {
        sparams.use_lut = true;
        sparams.lut_resolution = p->lut_resolution;
        sparams.lut_cache = &eng->lut_cache;  // memo the build (PERF, byte-identical)
    }
    sparams.tone_curve = build_tone_curve_set(p);
    // Scanner BLACK/WHITE XYZ correction (print route): apply the shared affine
    // (m, q) per pixel in scan(). Inactive (strict no-op) by default / when the
    // print paper is not negative.
    if (bw_corr.active) {
        sparams.bw_xyz_correction = true;
        sparams.bw_xyz_m = bw_corr.m;
        sparams.bw_xyz_q = bw_corr.q;
    }
    final_rgb->assign(static_cast<size_t>(npix) * 3, 0.0f);
    spk::scan(prnt, sparams, print_density_cmy.data(), width, height,
              final_rgb->data());
    if (cancellation_requested(cancel, cancel_user_data)) {
        final_rgb->clear();
        return SPK_ERR_CANCELLED;
    }
    return SPK_OK;
} catch (const spk::ParallelCancelled&) {
    if (final_rgb) final_rgb->clear();
    if (tap_log_raw) tap_log_raw->clear();
    if (tap_film_density_cmy) tap_film_density_cmy->clear();
    if (tap_print_density_cmy) tap_print_density_cmy->clear();
    return SPK_ERR_CANCELLED;
}

// Allocate an spk_image and copy `data` into it.
spk_status fill_out_image(spk_image* out, const std::vector<float>& data,
                          int width, int height, int color_space) {
    out->data = static_cast<float*>(std::malloc(data.size() * sizeof(float)));
    if (!out->data) return SPK_ERR_OOM;
    std::memcpy(out->data, data.data(), data.size() * sizeof(float));
    out->width = width;
    out->height = height;
    out->color_space = color_space;
    return SPK_OK;
}

// Simple bilinear downscale of an interleaved-RGB float image to fit within
// `max_size` on the longest edge. Used by spk_simulate_preview. Aspect ratio is
// preserved; if the image already fits, the input is copied unchanged.
void downscale_bilinear(const float* src, int sw, int sh,
                        int dw, int dh, std::vector<float>* dst) {
    dst->assign(static_cast<size_t>(dw) * dh * 3, 0.0f);
    const float sx = sw > 1 ? static_cast<float>(sw - 1) / (dw - 1 > 0 ? dw - 1 : 1) : 0.0f;
    const float sy = sh > 1 ? static_cast<float>(sh - 1) / (dh - 1 > 0 ? dh - 1 : 1) : 0.0f;
    for (int y = 0; y < dh; ++y) {
        float fy = y * sy;
        int y0 = static_cast<int>(fy);
        int y1 = y0 + 1 < sh ? y0 + 1 : y0;
        float wy = fy - y0;
        for (int x = 0; x < dw; ++x) {
            float fx = x * sx;
            int x0 = static_cast<int>(fx);
            int x1 = x0 + 1 < sw ? x0 + 1 : x0;
            float wx = fx - x0;
            for (int c = 0; c < 3; ++c) {
                float v00 = src[(static_cast<size_t>(y0) * sw + x0) * 3 + c];
                float v01 = src[(static_cast<size_t>(y0) * sw + x1) * 3 + c];
                float v10 = src[(static_cast<size_t>(y1) * sw + x0) * 3 + c];
                float v11 = src[(static_cast<size_t>(y1) * sw + x1) * 3 + c];
                float top = v00 + (v01 - v00) * wx;
                float bot = v10 + (v11 - v10) * wx;
                (*dst)[(static_cast<size_t>(y) * dw + x) * 3 + c] = top + (bot - top) * wy;
            }
        }
    }
}

}  // namespace

extern "C" {

int spk_gpu_scan_state(void) { return spk::gpu_scan_preview_state(); }

uint64_t spk_gpu_scan_frames(void) { return spk::gpu_scan_frames_rendered(); }

int spk_gpu_print_state(void) { return spk::gpu_print_expose_state(); }

uint64_t spk_gpu_print_frames(void) { return spk::gpu_print_frames_rendered(); }

uint64_t spk_diffusion_fft_fallbacks(void) {
    return spk::diffusion_fft_fallbacks();
}
void spk_diffusion_reset_fft_fallbacks(void) { spk::diffusion_reset_fft_fallbacks(); }

void spk_set_big_cores(int mode) { spk::parallel_set_big_cores(mode); }

int spk_big_core_count(void) { return spk::parallel_big_core_count(); }

int spk_stage_timings(char* buf, int cap) {
    return spk::stage_timings_format(buf, cap);
}

uint64_t spk_stage_timing_render_id(void) {
    return spk::stage_timing_render_id();
}

int spk_stage_timings_json(char* buf, int cap) {
    return spk::stage_timings_json_format(buf, cap);
}

void spk_default_params(spk_params* p) {
    if (!p) return;
    const char* film = p->film_profile;
    const char* print = p->print_profile;
    std::memset(p, 0, sizeof(*p));
    p->film_profile = film;
    p->print_profile = print;
    p->enlarger_illuminant = "TH-KG3";

    // camera
    p->exposure_compensation_ev = 0.0f;
    p->auto_exposure = 1;
    // Schema default metering = "center_weighted"; NULL selects it. (The JNI
    // marshaller may later read CameraParams.auto_exposure_method; leaving it
    // NULL keeps the schema default until then.)
    p->auto_exposure_method = nullptr;
    p->lens_blur_um = 0.0f;
    p->film_format_mm = 35.0f;
    p->camera_filter_uv[0] = 0.0f; p->camera_filter_uv[1] = 410.0f; p->camera_filter_uv[2] = 8.0f;
    p->camera_filter_ir[0] = 0.0f; p->camera_filter_ir[1] = 675.0f; p->camera_filter_ir[2] = 15.0f;
    p->camera_diffusion_active = 0;
    p->camera_diffusion_family = SPK_DIFFUSION_BLACK_PRO_MIST;
    p->camera_diffusion_strength = 0.5f;
    p->camera_diffusion_spatial_scale = 1.0f;
    p->camera_diffusion_halo_warmth = 0.0f;
    p->camera_diffusion_core_intensity = 1.0f;
    p->camera_diffusion_core_size = 1.0f;
    p->camera_diffusion_halo_intensity = 1.0f;
    p->camera_diffusion_halo_size = 1.0f;
    p->camera_diffusion_bloom_intensity = 1.0f;
    p->camera_diffusion_bloom_size = 1.0f;

    // enlarger
    p->y_filter_shift = 0.0f;
    p->m_filter_shift = 0.0f;
    p->preflash_exposure = 0.0f;
    p->normalize_print_exposure = 1;
    p->print_exposure = 1.0f;
    p->print_exposure_compensation = 1;
    p->y_filter_neutral = 55.0f;
    p->m_filter_neutral = 65.0f;
    p->c_filter_neutral = 0.0f;
    p->enlarger_lens_blur = 0.0f;
    p->preflash_y_filter_shift = 0.0f;
    p->preflash_m_filter_shift = 0.0f;
    p->enlarger_diffusion_active = 0;
    p->enlarger_diffusion_family = SPK_DIFFUSION_BLACK_PRO_MIST;
    p->enlarger_diffusion_strength = 0.5f;
    p->enlarger_diffusion_spatial_scale = 1.0f;
    p->enlarger_diffusion_halo_warmth = 0.0f;
    p->enlarger_diffusion_core_intensity = 1.0f;
    p->enlarger_diffusion_core_size = 1.0f;
    p->enlarger_diffusion_halo_intensity = 1.0f;
    p->enlarger_diffusion_halo_size = 1.0f;
    p->enlarger_diffusion_bloom_intensity = 1.0f;
    p->enlarger_diffusion_bloom_size = 1.0f;

    // scanner
    p->scanner_lens_blur = 0.0f;
    p->scanner_unsharp[0] = 0.7f; p->scanner_unsharp[1] = 0.7f;
    p->scanner_white_correction = 0;
    p->scanner_black_correction = 0;
    p->scanner_white_level = 0.98f;
    p->scanner_black_level = 0.01f;

    // film rendering toggles
    p->density_curve_gamma = 1.0f;
    p->grain_active = 1;
    p->halation_active = 1;
    p->dir_couplers_active = 1;
    p->glare_active = 1;

    // grain
    p->grain_sublayers_active = 1;
    p->grain_particle_area_um2 = 0.2f;
    p->grain_particle_scale[0] = 0.8f; p->grain_particle_scale[1] = 1.0f; p->grain_particle_scale[2] = 2.0f;
    p->grain_particle_scale_layers[0] = 2.5f; p->grain_particle_scale_layers[1] = 1.0f; p->grain_particle_scale_layers[2] = 0.5f;
    p->grain_density_min[0] = 0.07f; p->grain_density_min[1] = 0.08f; p->grain_density_min[2] = 0.12f;
    p->grain_uniformity[0] = 0.97f; p->grain_uniformity[1] = 0.97f; p->grain_uniformity[2] = 0.99f;
    p->grain_blur = 0.65f;
    p->grain_blur_dye_clouds_um = 1.0f;
    p->grain_micro_structure[0] = 0.2f; p->grain_micro_structure[1] = 30.0f;
    p->grain_n_sub_layers = 1;

    // halation
    p->halation_scatter_amount = 1.0f;
    p->halation_scatter_spatial_scale = 1.0f;
    p->halation_halation_amount = 1.0f;
    p->halation_halation_spatial_scale = 1.0f;
    p->halation_scatter_core_um[0] = 2.2f; p->halation_scatter_core_um[1] = 2.0f; p->halation_scatter_core_um[2] = 1.6f;
    p->halation_scatter_tail_um[0] = 9.3f; p->halation_scatter_tail_um[1] = 9.7f; p->halation_scatter_tail_um[2] = 9.1f;
    p->halation_scatter_tail_weight[0] = 0.78f; p->halation_scatter_tail_weight[1] = 0.65f; p->halation_scatter_tail_weight[2] = 0.67f;
    p->halation_boost_ev = 0.0f;
    p->halation_boost_range = 0.3f;
    p->halation_protect_ev = 4.0f;
    p->halation_strength[0] = 0.05f; p->halation_strength[1] = 0.015f; p->halation_strength[2] = 0.0f;
    p->halation_first_sigma_um[0] = 65.0f; p->halation_first_sigma_um[1] = 65.0f; p->halation_first_sigma_um[2] = 65.0f;
    p->halation_n_bounces = 3;
    p->halation_bounce_decay = 0.5f;
    p->halation_renormalize = 1;

    // DIR couplers
    p->dir_amount = 1.0f;
    p->dir_inhibition_samelayer = 1.0f;
    p->dir_inhibition_interlayer = 1.0f;
    p->dir_gamma_samelayer_rgb[0] = 0.341f; p->dir_gamma_samelayer_rgb[1] = 0.324f; p->dir_gamma_samelayer_rgb[2] = 0.273f;
    p->dir_gamma_interlayer_r_to_gb[0] = 0.355f; p->dir_gamma_interlayer_r_to_gb[1] = 0.305f;
    p->dir_gamma_interlayer_g_to_rb[0] = 0.154f; p->dir_gamma_interlayer_g_to_rb[1] = 0.358f;
    p->dir_gamma_interlayer_b_to_rg[0] = 0.171f; p->dir_gamma_interlayer_b_to_rg[1] = 0.225f;
    p->dir_diffusion_size_um = 20.0f;
    p->dir_diffusion_tail_um = 200.0f;
    p->dir_diffusion_tail_weight = 0.06f;

    // glare
    p->glare_percent = 0.03f;
    p->glare_roughness = 0.7f;
    p->glare_blur = 0.5f;
    p->print_glare_active = 1;
    p->print_glare_percent = 0.03f;
    p->print_glare_roughness = 0.7f;
    p->print_glare_blur = 0.5f;
    p->print_density_curve_gamma = 1.0f;

    // s023 print density-curve morph: OFF + identity (a strict no-op default).
    p->print_morph_active = 0;
    p->print_morph_gamma_factor = 1.0f;
    p->print_morph_gamma_factor_fast = 1.0f;
    p->print_morph_gamma_factor_slow = 1.0f;
    p->print_morph_gamma_factor_red = 1.0f;
    p->print_morph_gamma_factor_green = 1.0f;
    p->print_morph_gamma_factor_blue = 1.0f;
    p->print_morph_developer_exhaustion = 0.0f;

    // io
    p->scan_film = 0;
    p->output_color_space = SPK_CS_SRGB;
    p->output_cctf_encoding = 1;
    p->input_cctf_decoding = 0;
    p->input_color_space = "ProPhoto RGB";
    p->crop = 0;
    p->crop_center[0] = 0.5f; p->crop_center[1] = 0.5f;
    p->crop_size[0] = 0.1f; p->crop_size[1] = 0.1f;
    p->upscale_factor = 1.0f;

    // settings
    p->rgb_to_raw_method = SPK_RGB2RAW_HANATOS2025;
    p->apply_hanatos_window = 1;
    p->apply_hanatos_surface = 0;
    p->spectral_gaussian_blur = 0.0f;
    p->use_enlarger_lut = 0;
    p->use_scanner_lut = 0;
    p->lut_resolution = 17;
    p->preview_max_size = 640;
    p->neutral_print_filters_from_database = 1;

    // gamut compression: both OFF by the byte-identical sentinel (output kLegacyClip=0,
    // input kOff=0) => the render/export path is bit-exact with every existing golden.
    p->output_gamut_compress = 0;
    p->input_gamut_compress = 0;

    // tone curve: OFF / identity by default (strict no-op => goldens stay bit-exact).
    p->tone_curve_active = 0;
    p->tone_curve_master_n = 0;
    p->tone_curve_rgb_n[0] = 0;
    p->tone_curve_rgb_n[1] = 0;
    p->tone_curve_rgb_n[2] = 0;

    // runtime render control: buffer memos active by default (previews and every
    // existing caller unchanged); one-shot renders opt out (see spektra.h).
    p->disable_buffer_memos = 0;
}

const char* spk_status_str(spk_status s) {
    switch (s) {
        case SPK_OK:                    return "ok";
        case SPK_ERR_BAD_ARGS:          return "bad arguments";
        case SPK_ERR_PROFILE_NOT_FOUND: return "profile not found";
        case SPK_ERR_ASSET_IO:          return "asset I/O error";
        case SPK_ERR_OOM:               return "out of memory";
        case SPK_ERR_INTERNAL:          return "internal error";
        case SPK_ERR_PROFILE_INVALID:   return "invalid profile";
        case SPK_ERR_CANCELLED:         return "cancelled";
        default:                        return "unknown";
    }
}

const char* spk_last_error_message(void) { return g_last_error_message; }

spk_status spk_engine_create(const char* asset_dir, spk_engine** out) {
    clear_last_error_message();
    if (!out) return SPK_ERR_BAD_ARGS;
    if (!asset_dir) return SPK_ERR_BAD_ARGS;  // filesystem mode requires a dir.
    auto eng = std::make_unique<spk_engine>();
    eng->asset_dir = asset_dir;
    eng->profiles_dir = join_path(eng->asset_dir, "profiles");
    *out = eng.release();
    return SPK_OK;
}

spk_status spk_engine_create_asset_manager(void* aasset_manager, spk_engine** out) {
    clear_last_error_message();
    if (!out) return SPK_ERR_BAD_ARGS;
#ifdef __ANDROID__
    if (!aasset_manager) return SPK_ERR_BAD_ARGS;
    auto eng = std::make_unique<spk_engine>();
    eng->asset_mgr = static_cast<AAssetManager*>(aasset_manager);
    // asset_base defaults to "spektra" (the bundled tree lives at assets/spektra/).
    // FS dirs (asset_dir/profiles_dir) stay empty: spk_read_asset uses the AAsset
    // path when asset_mgr is non-null.
    *out = eng.release();
    return SPK_OK;
#else
    (void)aasset_manager;
    return SPK_ERR_BAD_ARGS;  // no AAssetManager off Android (host tests use FS mode).
#endif
}

void spk_engine_destroy(spk_engine* eng) { delete eng; }

spk_status spk_engine_list_profiles(spk_engine* eng, char* buf, size_t buf_len,
                                    size_t* needed) {
    clear_last_error_message();
    if (!eng) return SPK_ERR_BAD_ARGS;
    std::string list;
#ifdef __ANDROID__
    if (eng->use_asset_mgr()) {
        std::string dir = eng->asset_base.empty()
                              ? std::string("profiles")
                              : eng->asset_base + "/profiles";
        AAssetDir* ad = AAssetManager_openDir(eng->asset_mgr, dir.c_str());
        if (!ad) return SPK_ERR_ASSET_IO;
        const char* fname;
        while ((fname = AAssetDir_getNextFileName(ad)) != nullptr) {
            std::string name = fname;
            if (ends_with(name, ".json")) {
                if (!list.empty()) list += '\n';
                list += name.substr(0, name.size() - 5);  // strip ".json"
            }
        }
        AAssetDir_close(ad);
    } else
#endif
    {
        DIR* d = opendir(eng->profiles_dir.c_str());
        if (!d) return SPK_ERR_ASSET_IO;
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            std::string name = ent->d_name;
            if (ends_with(name, ".json")) {
                if (!list.empty()) list += '\n';
                list += name.substr(0, name.size() - 5);  // strip ".json"
            }
        }
        closedir(d);
    }

    size_t req = list.size() + 1;  // include NUL
    if (needed) *needed = req;
    if (!buf) return needed ? SPK_OK : SPK_ERR_BAD_ARGS;
    if (buf_len < req) return SPK_ERR_BAD_ARGS;
    std::memcpy(buf, list.c_str(), req);
    return SPK_OK;
}

spk_status spk_simulate(spk_engine* eng, const spk_image* in, const spk_params* p,
                        spk_image* out) {
    return spk_simulate_cancellable(eng, in, p, out, nullptr, nullptr);
}

spk_status spk_simulate_cancellable(spk_engine* eng, const spk_image* in,
                                    const spk_params* p, spk_image* out,
                                    spk_cancel_check cancel,
                                    void* cancel_user_data) {
    clear_last_error_message();
    // The C API itself cannot know whether an exact render is a user export,
    // magnifier, or another caller. JNI supplies that logical purpose through
    // an outer context; direct C callers get the neutral exact_render label.
    spk::ScopedRenderTiming timing(spk::RTK_EXACT_RENDER);
    // Validate every pointer before any deref, matching spk_simulate_preview /
    // spk_simulate_tap. `in` is read for width/height just below, so guard it
    // (and its data) here rather than relying on the run_* callees.
    if (out) *out = {};
    if (!eng || !p || !out || !valid_rgb_shape(in)) {
        return timing.finish(SPK_ERR_BAD_ARGS);
    }
    if (cancellation_requested(cancel, cancel_user_data)) {
        return timing.finish(SPK_ERR_CANCELLED);
    }
    // EXPERIMENTAL GPU export latch (#149 option B, #154). gpu_export routes the
    // scan stage through the GPU on export, gated by scan()'s self-check + CPU
    // fallback. Only SETS the internal latch (never clears), so a preview
    // funneling through here keeps its own allow_gpu_scan; a plain export
    // (gpu_export == 0) is byte-identical to the CPU path. tap/bake hard-zero
    // the latch, so this is the ONLY export path that can reach the GPU.
    spk_params gpu_export_params;
    if (p->gpu_export != 0 && p->allow_gpu_scan == 0) {
        gpu_export_params = *p;
        gpu_export_params.allow_gpu_scan = 1;
        p = &gpu_export_params;
    }
    std::vector<float> rgb;
    spk_status st;
    int ow = in->width, oh = in->height;
    if (p->scan_film) {
        st = run_scan_film(eng, in, p, &rgb, nullptr, nullptr, &ow, &oh,
                           cancel, cancel_user_data);
    } else {
        // Print (enlarger) route: filming -> printing -> scan(print).
        st = run_print(eng, in, p, &rgb, nullptr, nullptr, nullptr, &ow, &oh,
                       cancel, cancel_user_data);
    }
    if (st != SPK_OK) return timing.finish(st);
    if (cancellation_requested(cancel, cancel_user_data)) {
        return timing.finish(SPK_ERR_CANCELLED);
    }
    return timing.finish(fill_out_image(out, rgb, ow, oh,
                                        static_cast<int>(p->output_color_space)));
}

spk_status spk_simulate_preview(spk_engine* eng, const spk_image* in,
                                const spk_params* p, spk_image* out) {
    return spk_simulate_preview_cancellable(eng, in, p, out, nullptr, nullptr);
}

spk_status spk_simulate_preview_cancellable(spk_engine* eng, const spk_image* in,
                                            const spk_params* p, spk_image* out,
                                            spk_cancel_check cancel,
                                            void* cancel_user_data) {
    clear_last_error_message();
    spk::ScopedRenderTiming timing(spk::RTK_PREVIEW);
    if (out) *out = {};
    if (!eng || !p || !out || !valid_rgb_shape(in)) {
        return timing.finish(SPK_ERR_BAD_ARGS);
    }
    if (cancellation_requested(cancel, cancel_user_data)) {
        return timing.finish(SPK_ERR_CANCELLED);
    }
    // Proxy-approximate / export-exact (docs/PERF_ROADMAP.md): the interactive preview
    // runs the approximate LUT fast-path (the scanner density->log_xyz spectral integral
    // is PCHIP-interpolated through a 3D LUT instead of evaluated per pixel; error is
    // profile/domain dependent — see runtime/stages/scanning.h). spk_simulate keeps the
    // caller's explicit setting; the app's exact-export route keeps it off. We copy the
    // caller's params and force the LUT on for the preview only.
    spk_params pp = *p;
    // Force BOTH spectral LUTs on for the interactive preview (proxy-approximate / export-exact):
    // the scanner density->log_xyz integral AND the enlarger print-expose integral are the two
    // per-pixel hotspots, so PCHIP-interpolating both through their 3D LUTs roughly halves
    // the preview render on the print route. Scanner error is profile/domain dependent;
    // the enlarger gate remains ~1e-4 vs direct. The app's exact export keeps both LUTs off,
    // and the bit-exact parity goldens do not call simulate_preview. The enlarger LUT is
    // inert on the scan_film route.
    if (pp.use_scanner_lut == 0) pp.use_scanner_lut = 1;
    if (pp.use_enlarger_lut == 0) pp.use_enlarger_lut = 1;
    if (pp.lut_resolution < 2) pp.lut_resolution = 17;
    // GPU preview fast-path latch (#146): ONLY this preview entry translates the
    // user's gpu_preview toggle into allow_gpu_scan. No in-tree caller sets the
    // INTERNAL field anywhere else, spk_simulate_tap and spk_bake_cube_lut
    // hard-zero it defensively, and the JNI marshaller starts from
    // spk_default_params' memset — spk_simulate itself cannot clamp it because
    // this preview entry funnels through it, so for direct C callers of
    // spk_simulate the field's keep-0 contract is documented, not enforced
    // (law revision #149: preview-only until option-B ships). When scan() engages the GPU it skips the scanner LUT forced on
    // above entirely (the fp32 direct integral is tighter than the LUT).
    if (pp.gpu_preview != 0) pp.allow_gpu_scan = 1;
    // The preview funnels through spk_simulate, whose gpu_export latch must NOT
    // fire here — preview GPU use is governed solely by gpu_preview above. Clear
    // gpu_export on the preview copy so the two toggles stay independent.
    pp.gpu_export = 0;
    p = &pp;
    int max_size = p->preview_max_size > 0 ? p->preview_max_size : 640;
    int longest = in->width > in->height ? in->width : in->height;
    if (longest <= max_size) {
        // Already small enough: run the full simulate on the original.
        return timing.finish(spk_simulate_cancellable(
            eng, in, p, out, cancel, cancel_user_data));
    }
    // Downscale (bilinear) to the preview target, preserving aspect ratio, then
    // simulate. Done in input (linear ProPhoto) space, before the pipeline.
    double scale = static_cast<double>(max_size) / longest;
    int dw = in->width  > 1 ? static_cast<int>(std::lround(in->width  * scale)) : 1;
    int dh = in->height > 1 ? static_cast<int>(std::lround(in->height * scale)) : 1;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    std::vector<float> small;
    downscale_bilinear(in->data, in->width, in->height, dw, dh, &small);
    if (cancellation_requested(cancel, cancel_user_data)) {
        return timing.finish(SPK_ERR_CANCELLED);
    }

    spk_image small_img{small.data(), dw, dh, in->color_space};
    return timing.finish(spk_simulate_cancellable(
        eng, &small_img, p, out, cancel, cancel_user_data));
}

spk_status spk_simulate_tap(spk_engine* eng, const spk_image* in,
                            const spk_params* p, const char* tap_name,
                            spk_image* out) {
    clear_last_error_message();
    spk::ScopedRenderTiming timing(spk::RTK_TAP);
    if (!out || !p || !tap_name || !in) {
        return timing.finish(SPK_ERR_BAD_ARGS);
    }

    // Debug taps feed the parity harness: hard-zero the INTERNAL GPU latch so a
    // raw C caller's allow_gpu_scan can never put GPU output into a tap
    // (#146/#149 — same defensive posture as spk_bake_cube_lut).
    spk_params tp = *p;
    tp.allow_gpu_scan = 0;
    p = &tp;

    std::string tap = tap_name;
    std::vector<float> log_raw, film_density_cmy, print_density_cmy, final_rgb;
    spk_status st;
    // Output geometry after the crop/resize preprocess (== input geometry under
    // default params); written by the run_* functions.
    int ow = in->width, oh = in->height;

    if (p->scan_film) {
        // scan_film route: only the negative taps + final RGB exist.
        if (tap == "film_log_raw") {
            st = run_scan_film(eng, in, p, nullptr, &log_raw, nullptr, &ow, &oh);
            if (st != SPK_OK) return timing.finish(st);
            return timing.finish(
                fill_out_image(out, log_raw, ow, oh, in->color_space));
        } else if (tap == "film_density_cmy") {
            st = run_scan_film(eng, in, p, nullptr, &log_raw, &film_density_cmy,
                               &ow, &oh);
            if (st != SPK_OK) return timing.finish(st);
            return timing.finish(
                fill_out_image(out, film_density_cmy, ow, oh, in->color_space));
        } else if (tap == "final_rgb") {
            st = run_scan_film(eng, in, p, &final_rgb, nullptr, nullptr, &ow, &oh);
            if (st != SPK_OK) return timing.finish(st);
            return timing.finish(fill_out_image(
                out, final_rgb, ow, oh,
                static_cast<int>(p->output_color_space)));
        }
        return timing.finish(
            SPK_ERR_BAD_ARGS);  // unknown / print-only tap on scan_film route
    }

    // Print route taps (filming -> printing -> scan).
    if (tap == "film_log_raw") {
        st = run_print(eng, in, p, nullptr, &log_raw, nullptr, nullptr, &ow, &oh);
        if (st != SPK_OK) return timing.finish(st);
        return timing.finish(fill_out_image(out, log_raw, ow, oh, in->color_space));
    } else if (tap == "film_density_cmy") {
        st = run_print(eng, in, p, nullptr, nullptr, &film_density_cmy, nullptr,
                       &ow, &oh);
        if (st != SPK_OK) return timing.finish(st);
        return timing.finish(
            fill_out_image(out, film_density_cmy, ow, oh, in->color_space));
    } else if (tap == "print_density_cmy") {
        st = run_print(eng, in, p, nullptr, nullptr, nullptr, &print_density_cmy,
                       &ow, &oh);
        if (st != SPK_OK) return timing.finish(st);
        return timing.finish(
            fill_out_image(out, print_density_cmy, ow, oh, in->color_space));
    } else if (tap == "final_rgb") {
        st = run_print(eng, in, p, &final_rgb, nullptr, nullptr, nullptr, &ow, &oh);
        if (st != SPK_OK) return timing.finish(st);
        return timing.finish(fill_out_image(
            out, final_rgb, ow, oh,
            static_cast<int>(p->output_color_space)));
    }
    return timing.finish(SPK_ERR_BAD_ARGS);  // unknown tap
}

spk_status spk_meter_exposure_ev(spk_engine* eng, const spk_image* in,
                                 const spk_params* p, double* out_ev) {
    return spk_meter_exposure_ev_cancellable(
        eng, in, p, out_ev, nullptr, nullptr);
}

spk_status spk_meter_exposure_ev_cancellable(spk_engine* eng,
                                             const spk_image* in,
                                             const spk_params* p,
                                             double* out_ev,
                                             spk_cancel_check cancel,
                                             void* cancel_user_data) {
    clear_last_error_message();
    if (!eng || !p || !out_ev || !valid_rgb_shape(in)) return SPK_ERR_BAD_ARGS;
    if (!supported_input_color_space(p->input_color_space)) return SPK_ERR_BAD_ARGS;
    if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;

    // AE off => the render applies no gain, so the caller's gain is unity (0 EV).
    *out_ev = 0.0;
    if (p->auto_exposure == 0) return SPK_OK;

    const int w = in->width, h = in->height;

    // Method resolution is character-for-character the same as preprocess_geometry's
    // (NULL => the schema default center_weighted; an unrecognised string is passed
    // through as known=false so the metering collapses to 0 EV, matching the render).
    spk::AeMethod method = spk::AeMethod::kCenterWeighted;
    const bool known = p->auto_exposure_method == nullptr
                           ? true
                           : spk::ae_method_from_string(p->auto_exposure_method,
                                                        &method);

    // Meter straight off the caller's float32 frame — byte-identical EV to
    // apply_auto_exposure on a float64 copy (float->double widening is exact;
    // see measure_auto_exposure_ev_f32), without the full-resolution float64
    // scratch this entry point used to allocate (~288 MB at 12 MP,
    // EXPORT_FASTPATH item 4). Still the same metering code the render path
    // runs, so the value cannot drift from what the render actually uses.
    *out_ev = spk::measure_auto_exposure_ev_f32(
        in->data, w, h, spk::AeColorSpace::kProPhotoRGB,
        /*apply_cctf_decoding=*/p->input_cctf_decoding != 0, method, known);
    if (cancellation_requested(cancel, cancel_user_data)) return SPK_ERR_CANCELLED;
    return SPK_OK;
}

// sRGB EOTF: shaped lattice coordinate -> the linear value that coordinate stands for.
// The GPU side applies the inverse (linear -> encoded) before its lookup, so the two
// must stay exact inverses of each other.
static inline float shaper_to_linear(float e) {
    if (e <= 0.04045f) return e / 12.92f;
    return static_cast<float>(std::pow((e + 0.055f) / 1.055f, 2.4));
}

spk_status spk_bake_cube_lut(spk_engine* eng, const spk_params* p, int lut_size,
                             int32_t shaper,
                             char* out_text, size_t out_cap, size_t* needed) {
    return spk_bake_cube_lut_cancellable(
        eng, p, lut_size, shaper, out_text, out_cap, needed, nullptr, nullptr);
}

spk_status spk_bake_cube_lut_cancellable(
        spk_engine* eng, const spk_params* p, int lut_size, int32_t shaper,
        char* out_text, size_t out_cap, size_t* needed,
        spk_cancel_check cancel, void* cancel_user_data) {
    clear_last_error_message();
    spk::ScopedRenderTiming timing(spk::RTK_LUT_BAKE);
    if (needed) *needed = 0;
    if (!eng || !p) return timing.finish(SPK_ERR_BAD_ARGS);
    if (cancellation_requested(cancel, cancel_user_data)) {
        return timing.finish(SPK_ERR_CANCELLED);
    }

    // Clamp the lattice size to a sane range. 33 is the default; below 2 a 3D LUT
    // is meaningless, and >256 explodes the bake cost (N^3 lattice points).
    int n = lut_size > 0 ? lut_size : 33;
    if (n < 2) n = 2;
    if (n > 256) n = 256;

    // --- Force spatial/stochastic effects OFF for the bake -------------------
    // A 3D LUT is a pure pointwise RGB->RGB map; any spatial or stochastic
    // effect (grain, halation + its in-emulsion scatter, camera/enlarger
    // diffusion glare, DIR-coupler spatial diffusion, scanner unsharp) cannot be
    // represented and would make the lattice irreproducible. We copy the params
    // and disable those, keeping the pointwise color science intact: spectral
    // upsampling, density curves, pointwise DIR couplers, printing, scanning,
    // and the output color-space transform. (Spatial effects are PER-EFFECT
    // gated — each runs off its own params, zero = inert — so every spatial
    // field must be zeroed here individually; halation_active alone no longer
    // masters them.)
    spk_params bp = *p;
    // GPU latch hard-zeroed (#146/#149): a baked LUT is export-grade data, so it
    // must never render through the GPU regardless of what a raw C caller put in
    // the INTERNAL allow_gpu_scan field.
    bp.allow_gpu_scan = 0;
    bp.grain_active = 0;
    bp.halation_active = 0;
    bp.glare_active = 0;
    bp.print_glare_active = 0;
    bp.camera_diffusion_active = 0;
    bp.enlarger_diffusion_active = 0;
    bp.lens_blur_um = 0.0f;
    bp.dir_diffusion_size_um = 0.0f;  // DIR stays pointwise (size 0), like deactivate
    bp.scanner_lens_blur = 0.0f;
    bp.scanner_unsharp[0] = 0.0f;
    bp.scanner_unsharp[1] = 0.0f;
    // The .cube lattice is a synthetic count x 1 image; geometry transforms must
    // not touch it (a crop/rescale would corrupt the lattice -> wrong LUT).
    bp.crop = 0;
    bp.upscale_factor = 1.0f;
    // AUTO-EXPOSURE OFF — same reasoning as crop, and a correctness fix, not a
    // preference. auto_exposure defaults ON, and preprocess_geometry meters the
    // input to derive ONE global gain. Applied to the lattice it meters a
    // synthetic n^3 x 1 ramp of every RGB combination — not a photograph — and
    // bakes that meaningless gain into the LUT, which is then applied to real
    // images whose correct gain is unrelated. Because the film density curves are
    // non-linear, the resulting error is not a brightness offset: the scene lands
    // in the wrong region of the curve (the toe, where base fog lifts shadows and
    // contrast collapses), which is the underexposed/raised-blacks look reported
    // against the GPU LUT preview.
    //
    // A pointwise LUT fundamentally CANNOT carry auto-exposure: AE is a function
    // of the whole image, and the lattice is not an image. So the contract is that
    // the CALLER applies the exposure gain to its pixels BEFORE the LUT lookup;
    // this bake emits the pure pointwise transform at unity gain. See the
    // spk_bake_cube_lut docs in spektra.h.
    bp.auto_exposure = 0;

    // --- Build the identity lattice as an spk_image --------------------------
    // Lattice axes span [0,1] in the engine's linear working space (treated as
    // ProPhoto-linear by the filming expose). The image is laid out so that BLUE
    // varies fastest, then GREEN, then RED (blue-fastest / red-slowest), matching
    // the .cube data ordering emitted below. We pack the whole lattice into a
    // single image row (width = n^3, height = 1) and run it through the exact
    // same per-pixel pipeline spk_simulate uses.
    const size_t count = static_cast<size_t>(n) * n * n;
    std::vector<float> lattice(count * 3);
    const float denom = static_cast<float>(n - 1);
    // With a shaper the lattice is spaced evenly in the SHAPED domain, so the entries
    // are the linear values those shaped coordinates represent. That is what moves the
    // sampling resolution down into the shadows where the film curve actually bends.
    const bool shaped = shaper != 0;
    size_t idx = 0;
    for (int r = 0; r < n; ++r) {
        if (cancellation_requested(cancel, cancel_user_data)) {
            return timing.finish(SPK_ERR_CANCELLED);
        }
        float rv = static_cast<float>(r) / denom;
        if (shaped) rv = shaper_to_linear(rv);
        for (int g = 0; g < n; ++g) {
            float gv = static_cast<float>(g) / denom;
            if (shaped) gv = shaper_to_linear(gv);
            for (int b = 0; b < n; ++b) {
                float bv = static_cast<float>(b) / denom;
                if (shaped) bv = shaper_to_linear(bv);
                lattice[idx * 3 + 0] = rv;
                lattice[idx * 3 + 1] = gv;
                lattice[idx * 3 + 2] = bv;
                ++idx;
            }
        }
    }

    spk_image in_img{lattice.data(), static_cast<int32_t>(count), 1,
                     static_cast<int32_t>(SPK_CS_PROPHOTO)};

    // --- Run the pipeline (same route spk_simulate would take) ---------------
    std::vector<float> rgb;
    spk_status st;
    if (bp.scan_film) {
        st = run_scan_film(eng, &in_img, &bp, &rgb, nullptr, nullptr,
                           nullptr, nullptr, cancel, cancel_user_data);
    } else {
        st = run_print(eng, &in_img, &bp, &rgb, nullptr, nullptr, nullptr,
                       nullptr, nullptr, cancel, cancel_user_data);
    }
    if (st != SPK_OK) return timing.finish(st);
    if (rgb.size() != count * 3) return timing.finish(SPK_ERR_INTERNAL);
    if (cancellation_requested(cancel, cancel_user_data)) {
        return timing.finish(SPK_ERR_CANCELLED);
    }

    // --- Emit the .cube text -------------------------------------------------
    const char* cs_name = "sRGB";
    switch (bp.output_color_space) {
        case SPK_CS_SRGB:        cs_name = "sRGB"; break;
        case SPK_CS_ADOBE_RGB:   cs_name = "Adobe RGB (1998)"; break;
        case SPK_CS_PROPHOTO:    cs_name = "ProPhoto RGB"; break;
        case SPK_CS_REC2020:     cs_name = "Rec.2020"; break;
        case SPK_CS_ACES2065_1:  cs_name = "ACES2065-1"; break;
        case SPK_CS_LINEAR_SRGB: cs_name = "linear sRGB"; break;
    }
    const char* route = bp.scan_film ? "scan negative" : "negative -> print -> scan";

    std::string out;
    out.reserve(count * 33 + 1024);
    char line[256];

    out += "# Spektrafilm for Android — baked film-look 3D LUT (.cube)\n";
    out += "# Film modeling powered by spektrafilm (GPLv3).\n";
    std::snprintf(line, sizeof(line), "# Film profile: %s\n",
                  p->film_profile ? p->film_profile : "(none)");
    out += line;
    if (!bp.scan_film) {
        std::snprintf(line, sizeof(line), "# Print profile: %s\n",
                      p->print_profile ? p->print_profile : "(none)");
        out += line;
    }
    std::snprintf(line, sizeof(line), "# Pipeline route: %s\n", route);
    out += line;
    out += "# INPUT domain:  linear ProPhoto RGB in [0,1] (engine working space,\n";
    out += "#                the lattice axis fed to the filming expose).\n";
    std::snprintf(line, sizeof(line),
                  "# OUTPUT domain: %s, CCTF encoding %s.\n", cs_name,
                  bp.output_cctf_encoding ? "ON" : "OFF");
    out += line;
    out += "# EXCLUDED (cannot be captured by a 3D LUT, forced OFF for bake):\n";
    out += "#   grain, halation (+ in-emulsion scatter), camera/enlarger diffusion\n";
    out += "#   glare, DIR-coupler spatial diffusion, scanner unsharp mask.\n";
    out += "# KEPT: spectral upsampling, density curves, pointwise DIR couplers,\n";
    out += "#   printing, scanning, output color-space transform.\n";
    out += "# Data order: BLUE fastest, then GREEN, then RED.\n";

    std::snprintf(line, sizeof(line), "TITLE \"Spektrafilm %s\"\n",
                  p->film_profile ? p->film_profile : "look");
    out += line;
    std::snprintf(line, sizeof(line), "LUT_3D_SIZE %d\n", n);
    out += line;
    out += "DOMAIN_MIN 0.0 0.0 0.0\n";
    out += "DOMAIN_MAX 1.0 1.0 1.0\n";

    for (size_t i = 0; i < count; ++i) {
        if ((i & 1023u) == 0u &&
            cancellation_requested(cancel, cancel_user_data)) {
            return timing.finish(SPK_ERR_CANCELLED);
        }
        std::snprintf(line, sizeof(line), "%.6f %.6f %.6f\n",
                      static_cast<double>(rgb[i * 3 + 0]),
                      static_cast<double>(rgb[i * 3 + 1]),
                      static_cast<double>(rgb[i * 3 + 2]));
        out += line;
    }

    const size_t req = out.size() + 1;  // include NUL terminator
    if (cancellation_requested(cancel, cancel_user_data)) {
        return timing.finish(SPK_ERR_CANCELLED);
    }
    if (needed) *needed = req;
    // A null output with `needed` is the successful sizing-query form. Keeping
    // it successful lets a JNI two-pass bake remain one successful logical
    // timing context instead of poisoning it with an expected BAD_ARGS child.
    if (!out_text) {
        return timing.finish(needed ? SPK_OK : SPK_ERR_BAD_ARGS);
    }
    if (out_cap < req) return timing.finish(SPK_ERR_BAD_ARGS);
    std::memcpy(out_text, out.c_str(), req);
    return timing.finish(SPK_OK);
}

void spk_image_free(spk_image* img) {
    if (img && img->data) {
        std::free(img->data);
        img->data = nullptr;
        img->width = 0;
        img->height = 0;
    }
}

}  // extern "C"

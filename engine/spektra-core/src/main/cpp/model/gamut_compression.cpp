/*
 * Spektrafilm for Android — native engine: output gamut compression.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm. Ports utils/gamut_compression.py::reinhard_knee,
 * compress_rgb_aces_rgc, and compress_rgb_oklch_chroma (the OkLch output-gamut
 * chroma reduction). All math is in double precision to match the oracle
 * (NumPy float64); the knee uses std::pow, the same transcendental NumPy calls.
 */
#include "model/gamut_compression.h"

#include <cmath>
#include <limits>
#include <vector>

namespace spk {

double reinhard_knee(double d, double threshold, double limit, double power) {
    // gamut_compression.py::reinhard_knee: identity at/below threshold (the oracle's
    // `mask = d > threshold` is strict, so d == threshold returns d unchanged), and a
    // smooth Reinhard roll-off above it that asymptotes to `limit`.
    if (!(d > threshold)) return d;  // (!(>) also leaves NaN untouched, as np.where would)
    const double scale = limit - threshold;
    const double x = (d - threshold) / scale;
    const double y = x / std::pow(1.0 + std::pow(x, power), 1.0 / power);
    return threshold + scale * y;
}

void compress_pixel_aces_rgc(const double rgb[3], double threshold, double limit,
                             double power, double out[3]) {
    // gamut_compression.py::compress_rgb_aces_rgc, per pixel.
    //   ach = max(R,G,B)
    //   safe_ach = ach if ach > 1e-12 else 1.0
    //   d = (ach - c) / safe_ach   (per channel; d >= 0, and d > 1 iff c < 0)
    //   c' = ach * (1 - reinhard_knee(d))
    //   pixels with ach <= 1e-12 keep their original (near-black) values.
    const double ach = std::fmax(rgb[0], std::fmax(rgb[1], rgb[2]));
    if (!(ach > 1e-12)) {  // near-black (or non-finite ach) -> identity, matching np.where
        out[0] = rgb[0];
        out[1] = rgb[1];
        out[2] = rgb[2];
        return;
    }
    for (int c = 0; c < 3; ++c) {
        const double d = (ach - rgb[c]) / ach;  // safe_ach == ach here (ach > 1e-12)
        const double dc = reinhard_knee(d, threshold, limit, power);
        out[c] = ach * (1.0 - dc);
    }
}

void compress_rgb_aces_rgc(double* rgb, int npix, double threshold, double limit,
                           double power) {
    for (int p = 0; p < npix; ++p) {
        double* px = rgb + static_cast<long>(p) * 3;
        const double in[3] = {px[0], px[1], px[2]};
        compress_pixel_aces_rgc(in, threshold, limit, power, px);
    }
}

// ---------------------------------------------------------------------------
// Input-side: radial xy compression toward the visible spectral locus.
// ---------------------------------------------------------------------------
namespace {

// CIE 1931 2 deg spectral locus, 380..700 nm @ 5 nm, first vertex repeated (closed).
// Captured from colour-science via gamut_compression.py::spectral_locus_xy()
// (tools/parity/gen_gamut_in_golden.py); 66 vertices / 65 edges. Embedded as a
// constant so the radial compression reproduces the oracle bit-for-bit without
// bundling a CMF table — tests/test_gamut_in_xy.cpp re-verifies it against the oracle.
constexpr int kSpectralLocusVerts = 66;
constexpr double kSpectralLocusXy[kSpectralLocusVerts][2] = {
    {0.1741122344263416, 0.00496372598145272},
    {0.17400791751588918, 0.004980548622995036},
    {0.17380077262082788, 0.004915411905373405},
    {0.1735599065272137, 0.004923202577307893},
    {0.17333686548078078, 0.0047967434472668885},
    {0.17302096545549497, 0.004775050361859285},
    {0.17257655084880216, 0.004799301919720766},
    {0.1720866307552483, 0.0048325242180399484},
    {0.17140743386310872, 0.005102170973749332},
    {0.17030098877973637, 0.005788504996470994},
    {0.16887752067098927, 0.00690024388793052},
    {0.16689529035208048, 0.00855560636081898},
    {0.16441175637527494, 0.01085755827676388},
    {0.16110457958027466, 0.013793358821732412},
    {0.15664093257730702, 0.01770480499089134},
    {0.15098540837597124, 0.022740193291642986},
    {0.14396039603960398, 0.029702970297029722},
    {0.13550267119961157, 0.0398791214721278},
    {0.12411847672778563, 0.05780251337374045},
    {0.10959432361561011, 0.08684251118309427},
    {0.0912935070022711, 0.13270204248699013},
    {0.06870592129105556, 0.20072321772810214},
    {0.045390734674777715, 0.29497596460628756},
    {0.02345994254707948, 0.4127034790935206},
    {0.008168028004667443, 0.5384230705117518},
    {0.0038585209003215433, 0.6548231511254019},
    {0.013870246085011192, 0.750186428038777},
    {0.03885180240320428, 0.8120160213618158},
    {0.07430242477337495, 0.833803091340228},
    {0.11416071960667964, 0.8262069597811889},
    {0.15472206121571344, 0.8058635454256492},
    {0.1928760978777212, 0.781629216363077},
    {0.22961967264964017, 0.7543290899027438},
    {0.2657750849711837, 0.7243239249298064},
    {0.3016037993957512, 0.6923077623715741},
    {0.3373633328508564, 0.6588482901396886},
    {0.37310154386845756, 0.624450859796661},
    {0.40873625570642336, 0.5896068688595312},
    {0.44406246358233303, 0.5547139028085305},
    {0.47877479115758376, 0.5202023072114564},
    {0.5124863667817968, 0.48659078806085704},
    {0.5447865055948337, 0.45443411456883603},
    {0.5751513113651648, 0.42423223492490464},
    {0.6029327855757162, 0.3964966335729773},
    {0.6270365997638726, 0.37249114521841786},
    {0.6482331060136394, 0.35139491630502157},
    {0.6657635762380971, 0.33401065115476053},
    {0.680078849721707, 0.31974721706864556},
    {0.6915039729617021, 0.30834226055665565},
    {0.7006060606060607, 0.29930069930069925},
    {0.7079177916216642, 0.2920271089348396},
    {0.7140315971169937, 0.2859288735456499},
    {0.7190329416297438, 0.280934951518654},
    {0.7230316025730948, 0.27694835774834164},
    {0.7259923175416133, 0.2740076824583867},
    {0.7282717282717283, 0.27172827172827163},
    {0.7299690128375388, 0.27003098716246127},
    {0.7310893955845097, 0.2689106044154904},
    {0.7319932998324957, 0.26800670016750433},
    {0.7327188940092165, 0.2672811059907835},
    {0.7334169672259683, 0.2665830327740317},
    {0.7340473003123604, 0.2659526996876395},
    {0.7343901649951473, 0.2656098350048527},
    {0.7345916616426285, 0.2654083383573716},
    {0.7346900232582807, 0.2653099767417192},
    {0.1741122344263416, 0.00496372598145272},
};

// gamut_compression.py::_ray_polygon_distance: distance from `origin` along `dir` to
// the first intersection with the closed locus polygon, via parametric segment
// intersection. denom = dir.x*ey - dir.y*ex; valid iff |denom| > 1e-12; accept the
// hit iff t > 1e-9 and 0 <= s <= 1. Returns +inf on a miss (numpy t_min init = inf).
double ray_polygon_distance(const double origin[2], const double dir[2]) {
    double t_min = std::numeric_limits<double>::infinity();
    for (int k = 0; k < kSpectralLocusVerts - 1; ++k) {
        const double ax = kSpectralLocusXy[k][0], ay = kSpectralLocusXy[k][1];
        const double ex = kSpectralLocusXy[k + 1][0] - ax;
        const double ey = kSpectralLocusXy[k + 1][1] - ay;
        const double denom = dir[0] * ey - dir[1] * ex;
        if (std::fabs(denom) > 1e-12) {
            const double ox = origin[0] - ax, oy = origin[1] - ay;
            const double t = (-ox * ey + oy * ex) / denom;
            const double s = (-ox * dir[1] + oy * dir[0]) / denom;
            if (t > 1e-9 && s >= 0.0 && s <= 1.0 && t < t_min) t_min = t;
        }
    }
    return t_min;
}

}  // namespace

int spectral_locus_xy(const double** out_xy) {
    *out_xy = &kSpectralLocusXy[0][0];
    return kSpectralLocusVerts;
}

void compress_pixel_xy(const double xy[2], const double white_xy[2], double threshold,
                       double limit, double power, double out[2]) {
    // gamut_compression.py::compress_xy_radial, per point.
    //   delta = xy - white; dist = |delta|; dir = delta / fmax(dist, 1e-12)
    //   boundary = ray_polygon_distance(white, dir); d_norm = dist / fmax(boundary, 1e-12)
    //   new_xy = white + dir * (reinhard_knee(d_norm) * boundary)
    //   return where(dist < 1e-9, xy, new_xy)   (at-white passthrough)
    const double dx = xy[0] - white_xy[0];
    const double dy = xy[1] - white_xy[1];
    const double dist = std::sqrt(dx * dx + dy * dy);
    const double safe_dist = std::fmax(dist, 1e-12);
    const double dir[2] = {dx / safe_dist, dy / safe_dist};
    const double boundary = ray_polygon_distance(white_xy, dir);
    const double d_norm = dist / std::fmax(boundary, 1e-12);
    const double d_comp = reinhard_knee(d_norm, threshold, limit, power);
    // 0 * inf == NaN on a genuine ray miss (boundary == inf), as numpy propagates;
    // for an interior white_xy every ray hits, so boundary is finite in practice.
    const double scaled = d_comp * boundary;
    const double nx = white_xy[0] + dir[0] * scaled;
    const double ny = white_xy[1] + dir[1] * scaled;
    if (dist < 1e-9) {  // np.where((dist < 1e-9), xy, new_xy): at-white passthrough
        out[0] = xy[0];
        out[1] = xy[1];
    } else {
        out[0] = nx;
        out[1] = ny;
    }
}

void compress_xy_radial(double* xy, int npix, const double white_xy[2],
                        double threshold, double limit, double power) {
    for (int p = 0; p < npix; ++p) {
        double* q = xy + static_cast<long>(p) * 2;
        const double in[2] = {q[0], q[1]};
        compress_pixel_xy(in, white_xy, threshold, limit, power, q);
    }
}

// ---------------------------------------------------------------------------
// Output-side: OkLch perceptual chroma reduction to the output RGB cube
// (gamut_compression.py::compress_rgb_oklch_chroma + _build_polar_perceptual_c_max_table
// + _c_max_lookup + _compress_lightness, specialised to algorithm=="oklch").
// ---------------------------------------------------------------------------
namespace {

// π as the bit-exact double NumPy uses for np.pi (== M_PI); hex-literal so the h_grid
// endpoints are byte-identical to np.linspace(-np.pi, np.pi, ...) regardless of platform
// math.h ANSI guards.
constexpr double kPi = 0x1.921fb54442d18p+1;

// --- Oklab shared matrices (bit-exact hex, colour 0.4.7). vector_dot(M, v) == M @ v. ---
// M1: XYZ -> LMS ; M2: LMS' -> OkLab. Inverses are colour's MATRIX_1_LMS_TO_XYZ /
// MATRIX_2_LAB_TO_LMS, which equal np.linalg.inv(M1)/np.linalg.inv(M2) bit-for-bit.
constexpr double kOklabM1[3][3] = {
    {  0x1.a34b2ffffd19dp-1,  0x1.728d320078e3dp-2, -0x1.07e79a00e84a6p-3 },
    {  0x1.0e359a0122b3cp-5,  0x1.dbcec3ffc10d5p-1,  0x1.281ae60381493p-5 },
    {  0x1.8adb5bfc6d32ep-5,  0x1.0eb607ffccd61p-2,  0x1.4488360028552p-1 },
};
constexpr double kOklabM2[3][3] = {
    {  0x1.af02a3fe8a4fap-3,  0x1.9655120032aadp-1, -0x1.0add9bd572b38p-8 },
    {  0x1.fa5e1bfffde12p+0, -0x1.36dc1bffe5d3ep+1,  0x1.cd686fff371a5p-2 },
    {  0x1.a869680b729e0p-6,  0x1.90c776001f502p-1, -0x1.9e0ac0001353dp-1 },
};
constexpr double kOklabM1Inv[3][3] = {  // LMS -> XYZ
    {  0x1.3a1d946a3a87fp+0, -0x1.1d97f58537d01p-1,  0x1.20019ca670854p-2 },
    { -0x1.4c6ecd6633e58p-5,  0x1.1cbcddbfc1706p+0, -0x1.259671ec13145p-4 },
    { -0x1.38db94efa7dd7p-4, -0x1.af98f8c4a28d6p-2,  0x1.960ecaf5e947dp+0 },
};
constexpr double kOklabM2Inv[3][3] = {  // OkLab -> LMS'
    {  0x1.fffffff2b0a82p-1,  0x1.95d992fe38807p-2,  0x1.b9f75219cc80fp-3 },
    {  0x1.0000002625996p+0, -0x1.b0611710080c0p-4, -0x1.058bf485b8813p-4 },
    {  0x1.000000ead0f39p+0, -0x1.6e86f739b7095p-4, -0x1.4a9ecbd4621c4p+0 },
};

// --- Per-output-space native RGB<->XYZ (illuminant = the space's OWN whitepoint, NO
// chromatic adaptation to D65 — this is colour.RGB_to_XYZ / XYZ_to_RGB with
// illuminant=cs.whitepoint & cctf off). Indexed by spk_color_space (0..5). Index 5
// (LINEAR_SRGB) is byte-identical to index 0 (sRGB) because cctf is off. Bit-exact hex.
constexpr double kRgbToXyz[6][3][3] = {
    {  // [0] sRGB
        {  0x1.a64c2f837b4a2p-2,  0x1.6e2eb1c432ca5p-2,  0x1.71a9fbe76c8b3p-3 },
        {  0x1.b367a0f9096bcp-3,  0x1.6e2eb1c432ca5p-1,  0x1.27bb2fec56d5dp-4 },
        {  0x1.3c36113404ea5p-6,  0x1.e83e425aee632p-4,  0x1.e6a7ef9db22d1p-1 },
    },
    {  // [1] Adobe RGB (1998)
        {  0x1.27414a4d2b2c0p-1,  0x1.7c06e19b90ea9p-3,  0x1.817ebaf102363p-3 },
        {  0x1.3079e59f2ba9dp-2,  0x1.41355475a31a5p-1,  0x1.3463497b7414ap-4 },
        {  0x1.badc0980b2420p-6,  0x1.218bd66277c46p-4,  0x1.fb90ea9e6eeb7p-1 },
    },
    {  // [2] ProPhoto RGB
        {  0x1.986c226809d49p-1,  0x1.14e3bcd35a857p-3,  0x1.0068db8bac70fp-5  },
        {  0x1.26e978d4fdf3bp-2,  0x1.6c7e28240b780p-1,  0x1.a36e2eb1c4333p-14 },
        { -0x1.529d8bce9dd8dp-60, -0x1.7d26869097f3bp-61, 0x1.a6594af4f0d84p-1  },
    },
    {  // [3] ITU-R BT.2020
        {  0x1.461f5d84c18dbp-1,  0x1.282ce83acff98p-3,  0x1.59de44c9f941ap-3 },
        {  0x1.0d0148ccf66f1p-2,  0x1.5b22902fd967ep-1,  0x1.e5ccb69ab60a3p-5 },
        {  0x1.c3f85a235493dp-55, 0x1.cbf168a39d522p-6,  0x1.0f9cb77c699aep+0 },
    },
    {  // [4] ACES2065-1
        {  0x1.e7b4f2983be02p-1, -0x1.88f09f952796fp-56, 0x1.88eaa17e5206ap-14 },
        {  0x1.6038bdb33fb82p-2,  0x1.74d22fc5e7ec9p-1, -0x1.277474fc3e450p-4  },
        { -0x1.945c48566e5f4p-60, -0x1.2482d70c72d1ep-61, 0x1.02425e0661114p+0  },
    },
    {  // [5] LINEAR_SRGB (== [0])
        {  0x1.a64c2f837b4a2p-2,  0x1.6e2eb1c432ca5p-2,  0x1.71a9fbe76c8b3p-3 },
        {  0x1.b367a0f9096bcp-3,  0x1.6e2eb1c432ca5p-1,  0x1.27bb2fec56d5dp-4 },
        {  0x1.3c36113404ea5p-6,  0x1.e83e425aee632p-4,  0x1.e6a7ef9db22d1p-1 },
    },
};
constexpr double kXyzToRgb[6][3][3] = {
    {  // [0] sRGB
        {  0x1.9ecbfb15b573fp+1, -0x1.8985f06f69446p+0, -0x1.fe90ff9724746p-2 },
        { -0x1.f013a92a30553p-1,  0x1.e0346dc5d6388p+0,  0x1.53f7ced916876p-5 },
        {  0x1.c84b5dcc63f13p-5, -0x1.a1cac083126e9p-3,  0x1.0e978d4fdf3b6p+0 },
    },
    {  // [1] Adobe RGB (1998)
        {  0x1.0552d234eb9a1p+1, -0x1.2148fd9fd36f9p-1, -0x1.6100e6afcce1dp-2 },
        { -0x1.f04039abf3387p-1,  0x1.e03f91e646f15p+0,  0x1.5475a31a4bdbdp-5 },
        {  0x1.b866e43aa79bap-7, -0x1.e4cd74927913fp-4,  0x1.03e22e5de15cap+0 },
    },
    {  // [2] ProPhoto RGB
        {  0x1.589374bc6a7f0p+0, -0x1.05bc01a36e2ecp-2, -0x1.a29c779a6b50fp-5  },
        { -0x1.16d5cfaacd9e8p-1,  0x1.8219652bd3c36p+0,  0x1.4fdf3b645a1cep-6  },
        { -0x1.aab28f12ea173p-60, -0x1.e6fdffe5e01c0p-61, 0x1.36594af4f0d84p+0 },
    },
    {  // [3] ITU-R BT.2020
        {  0x1.b77673c6f9e49p+0, -0x1.6c34f641d9636p-2, -0x1.03727351a2d1bp-2 },
        { -0x1.5557a6bfc0412p-1,  0x1.9dd1b6ddf1d7cp+0,  0x1.025a1324e0e31p-6 },
        {  0x1.2102ecb55b896p-6, -0x1.5e607a2582443p-5,  0x1.e25b571e54eebp-1 },
    },
    {  // [4] ACES2065-1
        {  0x1.0cc06a33249a9p+0, -0x1.1b40fff904389p-55, -0x1.98e12f51c9fb9p-14 },
        { -0x1.fbce0088cee1ap-2,  0x1.5f91719ae1931p+0,  0x1.926424e351582p-4  },
        { -0x1.5ce4fb528d408p-60, -0x1.8e31f5b810833p-61, 0x1.fb85627086a78p-1  },
    },
    {  // [5] LINEAR_SRGB (== [0])
        {  0x1.9ecbfb15b573fp+1, -0x1.8985f06f69446p+0, -0x1.fe90ff9724746p-2 },
        { -0x1.f013a92a30553p-1,  0x1.e0346dc5d6388p+0,  0x1.53f7ced916876p-5 },
        {  0x1.c84b5dcc63f13p-5, -0x1.a1cac083126e9p-3,  0x1.0e978d4fdf3b6p+0 },
    },
};

// C_max table geometry (gamut_compression.py: _OKLCH_CMAX_TABLE_N_L / _N_H / _N_BISECT,
// with the OUTPUT-side L grid linspace(0.02, 1.0, 64) from _get_output_c_max_table).
constexpr int kOklchNL = 64;
constexpr int kOklchNH = 720;
constexpr int kOklchNBisect = 18;

// colour.algebra.spow: sign-preserving power. Negative LMS occur for OOG/negative-XYZ
// pixels, so the cube-root and cube must preserve sign (plain pow would NaN). spow(0)=0.
inline double spow(double a, double p) {
    if (a == 0.0) return 0.0;
    return std::copysign(std::pow(std::fabs(a), p), a);
}

inline void mat3_mul(const double M[3][3], const double v[3], double out[3]) {
    out[0] = M[0][0] * v[0] + M[0][1] * v[1] + M[0][2] * v[2];
    out[1] = M[1][0] * v[0] + M[1][1] * v[1] + M[1][2] * v[2];
    out[2] = M[2][0] * v[0] + M[2][1] * v[1] + M[2][2] * v[2];
}

// colour.XYZ_to_Oklab: LMS = M1 @ XYZ ; LMS' = spow(LMS, 1/3) ; OkLab = M2 @ LMS'.
inline void oklab_from_xyz(const double xyz[3], double lab[3]) {
    double lms[3];
    mat3_mul(kOklabM1, xyz, lms);
    const double lms_[3] = {spow(lms[0], 1.0 / 3.0), spow(lms[1], 1.0 / 3.0),
                            spow(lms[2], 1.0 / 3.0)};
    mat3_mul(kOklabM2, lms_, lab);
}

// colour.Oklab_to_XYZ: LMS' = M2inv @ OkLab ; LMS = spow(LMS', 3) ; XYZ = M1inv @ LMS.
inline void xyz_from_oklab(const double lab[3], double xyz[3]) {
    double lms_[3];
    mat3_mul(kOklabM2Inv, lab, lms_);
    const double lms[3] = {spow(lms_[0], 3.0), spow(lms_[1], 3.0), spow(lms_[2], 3.0)};
    mat3_mul(kOklabM1Inv, lms, xyz);
}

// Build the (kOklchNL x kOklchNH) C_max(L,h) table for output `space`, plus its grids.
// Ports _build_polar_perceptual_c_max_table for space=="oklch": L_grid=linspace(0.02,1,64)
// (inclusive; endpoint pinned to exactly 1.0 as np.linspace does), h_grid=linspace(-π,π,
// 720, endpoint=False), chroma upper 0.5, kOklchNBisect bisection iterations, in-gamut iff
// every native-RGB channel is in [-1e-6, 1+1e-6], returning the lower bracket `lo`.
void build_oklch_cmax_table(int space, double* table, double* L_grid, double* h_grid) {
    // np.linspace(0.02, 1.0, 64): y = arange(64) * step + 0.02 ; y[-1] := 1.0.
    const double Lstep = (1.0 - 0.02) / static_cast<double>(kOklchNL - 1);
    for (int i = 0; i < kOklchNL; ++i)
        L_grid[i] = static_cast<double>(i) * Lstep + 0.02;
    L_grid[kOklchNL - 1] = 1.0;  // np.linspace endpoint override
    // np.linspace(-π, π, 720, endpoint=False): step = 2π/720 ; y = arange(720)*step - π.
    const double hstart = -kPi;
    const double hstep = (kPi - (-kPi)) / static_cast<double>(kOklchNH);
    for (int j = 0; j < kOklchNH; ++j)
        h_grid[j] = static_cast<double>(j) * hstep + hstart;

    const double(*M)[3] = kXyzToRgb[space];
    for (int i = 0; i < kOklchNL; ++i) {
        const double L = L_grid[i];
        for (int j = 0; j < kOklchNH; ++j) {
            // np.cos/np.sin(h_mesh) are constant across the bisection; hoist them.
            const double ch = std::cos(h_grid[j]);
            const double sh = std::sin(h_grid[j]);
            double lo = 0.0;
            double hi = 0.5;
            for (int it = 0; it < kOklchNBisect; ++it) {
                const double mid = (lo + hi) * 0.5;
                const double lab[3] = {L, mid * ch, mid * sh};
                double xyz[3];
                xyz_from_oklab(lab, xyz);
                double rgb[3];
                mat3_mul(M, xyz, rgb);
                const bool in_gamut =
                    rgb[0] >= -1e-6 && rgb[0] <= 1.0 + 1e-6 &&
                    rgb[1] >= -1e-6 && rgb[1] <= 1.0 + 1e-6 &&
                    rgb[2] >= -1e-6 && rgb[2] <= 1.0 + 1e-6;
                if (in_gamut) lo = mid; else hi = mid;
            }
            table[static_cast<size_t>(i) * kOklchNH + j] = lo;
        }
    }
}

// Bilinear C_max lookup with hue wrap (gamut_compression.py::_c_max_lookup). L is clipped
// to the grid; h wraps mod kOklchNH (index 719 <-> 0). h_step and the L denominator are
// taken from the stored grid values (not the analytic step) to match the oracle exactly.
double cmax_lookup(double L, double h, const double* table, const double* L_grid,
                   const double* h_grid) {
    if (L < L_grid[0]) L = L_grid[0];
    else if (L > L_grid[kOklchNL - 1]) L = L_grid[kOklchNL - 1];

    const double h_step = h_grid[1] - h_grid[0];
    const double h_idx = (h - h_grid[0]) / h_step;
    const double h_floor = std::floor(h_idx);
    int h_lo = static_cast<int>(h_floor);
    h_lo = ((h_lo % kOklchNH) + kOklchNH) % kOklchNH;  // numpy floored modulo
    const int h_hi = (h_lo + 1) % kOklchNH;
    const double h_frac = h_idx - h_floor;

    const double L_idx = (L - L_grid[0]) / (L_grid[kOklchNL - 1] - L_grid[0]) *
                         static_cast<double>(kOklchNL - 1);
    int L_lo = static_cast<int>(std::floor(L_idx));
    if (L_lo < 0) L_lo = 0;
    else if (L_lo > kOklchNL - 2) L_lo = kOklchNL - 2;
    const int L_hi = L_lo + 1;
    const double L_frac = L_idx - static_cast<double>(L_lo);

    const double v00 = table[static_cast<size_t>(L_lo) * kOklchNH + h_lo];
    const double v01 = table[static_cast<size_t>(L_lo) * kOklchNH + h_hi];
    const double v10 = table[static_cast<size_t>(L_hi) * kOklchNH + h_lo];
    const double v11 = table[static_cast<size_t>(L_hi) * kOklchNH + h_hi];
    const double t0 = v00 * (1.0 - L_frac) * (1.0 - h_frac);
    const double t1 = v01 * (1.0 - L_frac) * h_frac;
    const double t2 = v10 * L_frac * (1.0 - h_frac);
    const double t3 = v11 * L_frac * h_frac;
    return t0 + t1 + t2 + t3;
}

// One linear-RGB triple -> compressed triple, given a prebuilt C_max table + grids for
// `space`. Mirrors compress_rgb_oklch_chroma per pixel (with lightness_compression pinned
// to (0.7,1.0,2.2), L_white=1). `out` may alias `rgb_in`.
void compress_pixel_oklch(const double rgb_in[3], int space, const double* table,
                          const double* L_grid, const double* h_grid, double threshold,
                          double limit, double power, double out[3]) {
    double xyz[3];
    mat3_mul(kRgbToXyz[space], rgb_in, xyz);
    double lab[3];
    oklab_from_xyz(xyz, lab);
    const double a = lab[1];
    const double b = lab[2];

    // _compress_lightness(L, (0.7,1.0,2.2), L_white=1.0): L/1 and *1 are exact no-ops,
    // so this reduces to the one-sided reinhard knee on L. C_max lookup + reconstruction
    // use this COMPRESSED L; C and h stay on the ORIGINAL a,b.
    const double L = reinhard_knee(lab[0], 0.7, 1.0, 2.2);
    const double C = std::hypot(a, b);
    const double h = std::atan2(b, a);

    const double C_max = cmax_lookup(L, h, table, L_grid, h_grid);
    const double safe_C_max = std::fmax(C_max, 1e-9);
    const double d_norm = C / safe_C_max;
    const double d_comp = reinhard_knee(d_norm, threshold, limit, power);
    const double C_new = d_comp * safe_C_max;

    const double a_new = C_new * std::cos(h);
    const double b_new = C_new * std::sin(h);
    const double lab_new[3] = {L, a_new, b_new};
    double xyz_new[3];
    xyz_from_oklab(lab_new, xyz_new);
    mat3_mul(kXyzToRgb[space], xyz_new, out);
}

// ---------------------------------------------------------------------------
// Oklrab: OkLch chroma reduction indexed by Ottosson's rebased lightness Lr.
// (gamut_compression.py::compress_rgb_oklrab_chroma + the "oklrab" branch of
// _get_output_c_max_table: same _build_polar_perceptual_c_max_table, but the polar
// coord is (Lr, a, b) and the reconstruction inverts Lr->L before Oklab_to_XYZ.)
// ---------------------------------------------------------------------------

// Ottosson 2023 OkLab "Lr" — a 1D nonlinear remap of OkLab's L so the lightness scale
// tracks CIELAB L* more closely. https://bottosson.github.io/posts/colorpicker/
// (gamut_compression.py::_OKLRAB_K1/_K2/_K3.)
constexpr double kOklrabK1 = 0.206;
constexpr double kOklrabK2 = 0.03;
constexpr double kOklrabK3 = (1.0 + kOklrabK1) / (1.0 + kOklrabK2);

// Forward L -> Lr (gamut_compression.py::_oklab_L_to_oklrab_Lr):
//   t = k3*L - k1 ; Lr = 0.5*(t + sqrt(t*t + 4*k2*k3*L)). Monotonic on [0,1], Lr(0)=0.
inline double oklab_L_to_oklrab_Lr(double L) {
    const double t = kOklrabK3 * L - kOklrabK1;
    return 0.5 * (t + std::sqrt(t * t + 4.0 * kOklrabK2 * kOklrabK3 * L));
}

// Inverse Lr -> L (gamut_compression.py::_oklrab_Lr_to_oklab_L):
//   L = (Lr*(Lr + k1)) / (k3*(Lr + k2)).
inline double oklrab_Lr_to_oklab_L(double Lr) {
    return (Lr * (Lr + kOklrabK1)) / (kOklrabK3 * (Lr + kOklrabK2));
}

// Build the (kOklchNL x kOklchNH) C_max(Lr,h) table for output `space`, plus its grids.
// Identical geometry to build_oklch_cmax_table (L_grid=linspace(0.02,1,64) inclusive,
// h_grid=linspace(-π,π,720,endpoint=False), chroma upper 0.5, kOklchNBisect iterations,
// in-gamut iff every native-RGB channel is in [-1e-6,1+1e-6]) — but the L axis IS Lr, so
// each grid row's OkLab lightness is recovered by oklrab_Lr_to_oklab_L before Oklab->XYZ.
// The stored L_grid keeps the Lr values so the per-pixel cmax_lookup can index by Lr.
void build_oklrab_cmax_table(int space, double* table, double* L_grid, double* h_grid) {
    const double Lstep = (1.0 - 0.02) / static_cast<double>(kOklchNL - 1);
    for (int i = 0; i < kOklchNL; ++i)
        L_grid[i] = static_cast<double>(i) * Lstep + 0.02;  // Lr grid
    L_grid[kOklchNL - 1] = 1.0;  // np.linspace endpoint override
    const double hstart = -kPi;
    const double hstep = (kPi - (-kPi)) / static_cast<double>(kOklchNH);
    for (int j = 0; j < kOklchNH; ++j)
        h_grid[j] = static_cast<double>(j) * hstep + hstart;

    const double(*M)[3] = kXyzToRgb[space];
    for (int i = 0; i < kOklchNL; ++i) {
        // The grid value is Lr; the OkLab L for the bisection reconstruction is its
        // inverse remap (oracle's polar_to_xyz_unit for the "oklrab" table).
        const double L = oklrab_Lr_to_oklab_L(L_grid[i]);
        for (int j = 0; j < kOklchNH; ++j) {
            const double ch = std::cos(h_grid[j]);
            const double sh = std::sin(h_grid[j]);
            double lo = 0.0;
            double hi = 0.5;
            for (int it = 0; it < kOklchNBisect; ++it) {
                const double mid = (lo + hi) * 0.5;
                const double lab[3] = {L, mid * ch, mid * sh};
                double xyz[3];
                xyz_from_oklab(lab, xyz);
                double rgb[3];
                mat3_mul(M, xyz, rgb);
                const bool in_gamut =
                    rgb[0] >= -1e-6 && rgb[0] <= 1.0 + 1e-6 &&
                    rgb[1] >= -1e-6 && rgb[1] <= 1.0 + 1e-6 &&
                    rgb[2] >= -1e-6 && rgb[2] <= 1.0 + 1e-6;
                if (in_gamut) lo = mid; else hi = mid;
            }
            table[static_cast<size_t>(i) * kOklchNH + j] = lo;
        }
    }
}

// One linear-RGB triple -> compressed triple via the Lr-indexed reduction. Mirrors
// compress_pixel_oklch except the C_max lookup axis is Lr = f(L); the reconstruction
// still preserves the (lightness-compressed) OkLab L. `out` may alias `rgb_in`.
void compress_pixel_oklrab(const double rgb_in[3], int space, const double* table,
                           const double* L_grid, const double* h_grid, double threshold,
                           double limit, double power, double out[3]) {
    double xyz[3];
    mat3_mul(kRgbToXyz[space], rgb_in, xyz);
    double lab[3];
    oklab_from_xyz(xyz, lab);
    const double a = lab[1];
    const double b = lab[2];

    // One-sided lightness knee on L (pinned (0.7,1.0,2.2), L_white=1) runs BEFORE the Lr
    // remap; the C_max lookup uses Lr, but C, h, and the reconstruction use this L.
    const double L = reinhard_knee(lab[0], 0.7, 1.0, 2.2);
    const double Lr = oklab_L_to_oklrab_Lr(L);
    const double C = std::hypot(a, b);
    const double h = std::atan2(b, a);

    const double C_max = cmax_lookup(Lr, h, table, L_grid, h_grid);  // indexed by Lr
    const double safe_C_max = std::fmax(C_max, 1e-9);
    const double d_norm = C / safe_C_max;
    const double d_comp = reinhard_knee(d_norm, threshold, limit, power);
    const double C_new = d_comp * safe_C_max;

    const double a_new = C_new * std::cos(h);
    const double b_new = C_new * std::sin(h);
    const double lab_new[3] = {L, a_new, b_new};  // reconstruct on L, not Lr
    double xyz_new[3];
    xyz_from_oklab(lab_new, xyz_new);
    mat3_mul(kXyzToRgb[space], xyz_new, out);
}


// ---------------------------------------------------------------------------
// Jzazbz: JzCzhz perceptual chroma reduction (Safdar 2017, colour-science 0.4.7).
// (gamut_compression.py::compress_rgb_jzazbz_chroma + the "jzazbz" branch of
// _get_output_c_max_table.) Same algorithm shape as OkLch; the perceptual space is
// JzAzBz at an absolute reference white of Y_w = 100 cd/m^2 (_JZAZBZ_Y_W_CDM2):
// linear RGB=1 maps to Y=100 before the forward, undone after the inverse.
// ---------------------------------------------------------------------------

// colour.models.jzazbz constants (Safdar 2017) + the ST 2084 PQ constants with the
// re-optimized m2 = 1.7 * 2523 / 2^5. Decimal literals match the colour source; the
// inverse matrices are np.linalg.inv results captured as exact float64 hex.
constexpr double kJzB = 1.15;
constexpr double kJzG = 0.66;
constexpr double kJzD = -0.56;
constexpr double kJzD0 = 1.6295499532821566e-11;
constexpr double kJzYw = 100.0;                       // _JZAZBZ_Y_W_CDM2
constexpr double kPqM1 = 0x1.4640000000000p-3;        // 2610/4096/4
constexpr double kPqM2Jz = 0x1.0c11999999999p+7;      // 1.7 * 2523/4096 * 128
constexpr double kPqC1 = 0x1.ac00000000000p-1;        // 3424/4096
constexpr double kPqC2 = 0x1.2da0000000000p+4;        // 2413/4096*32
constexpr double kPqC3 = 0x1.2b00000000000p+4;        // 2392/4096*32
constexpr double kJzMXyzToLms[3][3] = {
    {0.41478972, 0.579999, 0.0146480},
    {-0.2015100, 1.120649, 0.0531008},
    {-0.0166008, 0.264800, 0.6684799},
};
constexpr double kJzMLmsToXyz[3][3] = {  // np.linalg.inv(kJzMXyzToLms), exact hex
    {  0x1.ec9a1a8bce714p+0, -0x1.013a11a9de8acp+0,  0x1.3470b79eb8366p-5 },
    {  0x1.66b96ff1c1292p-2,  0x1.73f557d230e47p-1, -0x1.0bd08963ad7e9p-4 },
    { -0x1.74aa645ab6307p-4, -0x1.403bd8515285fp-2,  0x1.85d407843f9bep+0 },
};
constexpr double kJzMLmspToIzazbz[3][3] = {
    {0.500000, 0.500000, 0.000000},
    {3.524000, -4.066708, 0.542708},
    {0.199076, 1.096799, -1.295875},
};
constexpr double kJzMIzazbzToLmsp[3][3] = {  // np.linalg.inv(kJzMLmspToIzazbz), exact hex
    {  0x1.0000000000000p+0,  0x1.1bdcf5ff4b9ffp-3,  0x1.db860b905af43p-5 },
    {  0x1.0000000000000p+0, -0x1.1bdcf5ff4b9fep-3, -0x1.db860b905af50p-5 },
    {  0x1.0000000000000p+0, -0x1.894b7904a2cf8p-4, -0x1.9fb04b6ae56fdp-1 },
};

// The output color space's whitepoint as XYZ at Y = 1
// (gamut_compression.py::_output_cs_whitepoint_xyz / _xy_to_xyz_unit_y), captured as
// exact float64 hex from colour.RGB_COLOURSPACES[...].whitepoint. Indexed by
// spk_color_space; [5] LINEAR_SRGB == [0] sRGB.
constexpr double kWhitepointXyzUnitY[6][3] = {
    {  0x1.e6a228c5f3dc7p-1,  0x1.0000000000000p+0,  0x1.16cc7d1ef8103p+0 },  // sRGB D65
    {  0x1.e6a228c5f3dc7p-1,  0x1.0000000000000p+0,  0x1.16cc7d1ef8103p+0 },  // Adobe D65
    {  0x1.edb829b3e0ddbp-1,  0x1.0000000000000p+0,  0x1.a6741c471f7dcp-1 },  // ProPhoto D50
    {  0x1.e6a228c5f3dc7p-1,  0x1.0000000000000p+0,  0x1.16cc7d1ef8103p+0 },  // BT.2020 D65
    {  0x1.e7c139ede16abp-1,  0x1.0000000000000p+0,  0x1.02425e062bd5ep+0 },  // ACES ~D60
    {  0x1.e6a228c5f3dc7p-1,  0x1.0000000000000p+0,  0x1.16cc7d1ef8103p+0 },  // LINEAR_SRGB
};

// colour.eotf_inverse_ST2084 with the Jzazbz m2 (L_p = 10000).
inline double pq_encode_jz(double c) {
    const double y = spow(c / 10000.0, kPqM1);
    return spow((kPqC1 + kPqC2 * y) / (kPqC3 * y + 1.0), kPqM2Jz);
}

// colour.eotf_ST2084 with the Jzazbz m2 (L_p = 10000).
inline double pq_decode_jz(double n) {
    const double v = spow(n, 1.0 / kPqM2Jz);
    const double num = std::fmax(0.0, v - kPqC1);
    return 10000.0 * spow(num / (kPqC2 - kPqC3 * v), 1.0 / kPqM1);
}

// colour.XYZ_to_Jzazbz (Safdar 2017). `xyz` is absolute (cd/m^2, i.e. unit XYZ * 100).
inline void jzazbz_from_xyz(const double xyz[3], double jab[3]) {
    const double xp = kJzB * xyz[0] - (kJzB - 1.0) * xyz[2];
    const double yp = kJzG * xyz[1] - (kJzG - 1.0) * xyz[0];
    const double xyz_p[3] = {xp, yp, xyz[2]};
    double lms[3];
    mat3_mul(kJzMXyzToLms, xyz_p, lms);
    const double lms_p[3] = {pq_encode_jz(lms[0]), pq_encode_jz(lms[1]),
                             pq_encode_jz(lms[2])};
    double iab[3];
    mat3_mul(kJzMLmspToIzazbz, lms_p, iab);
    jab[0] = ((1.0 + kJzD) * iab[0]) / (1.0 + kJzD * iab[0]) - kJzD0;
    jab[1] = iab[1];
    jab[2] = iab[2];
}

// colour.Jzazbz_to_XYZ (Safdar 2017). Returns absolute XYZ (cd/m^2).
inline void xyz_from_jzazbz(const double jab[3], double xyz[3]) {
    const double jz = jab[0] + kJzD0;
    const double iz = jz / (1.0 + kJzD - kJzD * jz);
    const double iab[3] = {iz, jab[1], jab[2]};
    double lms_p[3];
    mat3_mul(kJzMIzazbzToLmsp, iab, lms_p);
    const double lms[3] = {pq_decode_jz(lms_p[0]), pq_decode_jz(lms_p[1]),
                           pq_decode_jz(lms_p[2])};
    double xyz_p[3];
    mat3_mul(kJzMLmsToXyz, lms, xyz_p);
    const double x = (xyz_p[0] + (kJzB - 1.0) * xyz_p[2]) / kJzB;
    const double y = (xyz_p[1] + (kJzG - 1.0) * x) / kJzG;
    xyz[0] = x;
    xyz[1] = y;
    xyz[2] = xyz_p[2];
}

// gamut_compression.py::_jzazbz_white_Jz: Jz of the output whitepoint at Y_w cd/m^2.
double jzazbz_white_Jz(int space) {
    const double xyz_abs[3] = {kWhitepointXyzUnitY[space][0] * kJzYw,
                               kWhitepointXyzUnitY[space][1] * kJzYw,
                               kWhitepointXyzUnitY[space][2] * kJzYw};
    double jab[3];
    jzazbz_from_xyz(xyz_abs, jab);
    return jab[0];
}

// _get_output_c_max_table("jzazbz", ...): Jz grid linspace(0.002, 0.18, 64) (endpoint
// pinned), chroma upper 0.3, same 720-hue/18-bisection geometry, in-gamut iff every
// native-RGB channel of Jzazbz_to_XYZ(jab)/Y_w is in [-1e-6, 1+1e-6].
void build_jzazbz_cmax_table(int space, double* table, double* L_grid, double* h_grid) {
    const double Lstart = 0.002;
    const double Lend = 0.18;
    const double Lstep = (Lend - Lstart) / static_cast<double>(kOklchNL - 1);
    for (int i = 0; i < kOklchNL; ++i)
        L_grid[i] = static_cast<double>(i) * Lstep + Lstart;
    L_grid[kOklchNL - 1] = Lend;  // np.linspace endpoint override
    const double hstart = -kPi;
    const double hstep = (kPi - (-kPi)) / static_cast<double>(kOklchNH);
    for (int j = 0; j < kOklchNH; ++j)
        h_grid[j] = static_cast<double>(j) * hstep + hstart;

    const double(*M)[3] = kXyzToRgb[space];
    for (int i = 0; i < kOklchNL; ++i) {
        const double Jz = L_grid[i];
        for (int j = 0; j < kOklchNH; ++j) {
            const double ch = std::cos(h_grid[j]);
            const double sh = std::sin(h_grid[j]);
            double lo = 0.0;
            double hi = 0.3;
            for (int it = 0; it < kOklchNBisect; ++it) {
                const double mid = (lo + hi) * 0.5;
                const double jab[3] = {Jz, mid * ch, mid * sh};
                double xyz_abs[3];
                xyz_from_jzazbz(jab, xyz_abs);
                const double xyz[3] = {xyz_abs[0] / kJzYw, xyz_abs[1] / kJzYw,
                                       xyz_abs[2] / kJzYw};
                double rgb[3];
                mat3_mul(M, xyz, rgb);
                const bool in_gamut =
                    rgb[0] >= -1e-6 && rgb[0] <= 1.0 + 1e-6 &&
                    rgb[1] >= -1e-6 && rgb[1] <= 1.0 + 1e-6 &&
                    rgb[2] >= -1e-6 && rgb[2] <= 1.0 + 1e-6;
                if (in_gamut) lo = mid; else hi = mid;
            }
            table[static_cast<size_t>(i) * kOklchNH + j] = lo;
        }
    }
}

// compress_rgb_jzazbz_chroma per pixel (lightness_compression pinned (0.7,1.0,2.2),
// L_white = the output whitepoint's Jz). `out` may alias `rgb_in`.
void compress_pixel_jzazbz(const double rgb_in[3], int space, const double* table,
                           const double* L_grid, const double* h_grid, double Jz_white,
                           double threshold, double limit, double power, double out[3]) {
    double xyz_unit[3];
    mat3_mul(kRgbToXyz[space], rgb_in, xyz_unit);
    const double xyz_abs[3] = {xyz_unit[0] * kJzYw, xyz_unit[1] * kJzYw,
                               xyz_unit[2] * kJzYw};
    double jab[3];
    jzazbz_from_xyz(xyz_abs, jab);
    const double az = jab[1];
    const double bz = jab[2];

    // _compress_lightness(Jz, (0.7,1.0,2.2), L_white=Jz_white): normalize, knee,
    // denormalize. Black (Jz=0) passes through; C/h come from the ORIGINAL az,bz.
    const double Jz = reinhard_knee(jab[0] / Jz_white, 0.7, 1.0, 2.2) * Jz_white;
    const double Cz = std::hypot(az, bz);
    const double hz = std::atan2(bz, az);

    const double Cz_max = cmax_lookup(Jz, hz, table, L_grid, h_grid);
    const double safe_Cz_max = std::fmax(Cz_max, 1e-9);
    const double d_norm = Cz / safe_Cz_max;
    const double d_comp = reinhard_knee(d_norm, threshold, limit, power);
    const double Cz_new = d_comp * safe_Cz_max;

    const double jab_new[3] = {Jz, Cz_new * std::cos(hz), Cz_new * std::sin(hz)};
    double xyz_new_abs[3];
    xyz_from_jzazbz(jab_new, xyz_new_abs);
    const double xyz_new[3] = {xyz_new_abs[0] / kJzYw, xyz_new_abs[1] / kJzYw,
                               xyz_new_abs[2] / kJzYw};
    mat3_mul(kXyzToRgb[space], xyz_new, out);
}

// ---------------------------------------------------------------------------
// CAM16-UCS: CIECAM16 Uniform Color Space chroma reduction (Li et al. 2017, via
// colour-science 0.4.7). (gamut_compression.py::compress_rgb_cam16ucs_chroma + the
// "cam16ucs" branch of _get_output_c_max_table.) The adapting whitepoint is the
// output space's whitepoint at Y=1 (x100 inside the model); viewing conditions are
// fixed at L_A = 64 cd/m^2, Y_b = 20, Average surround (F=1, c=0.69, N_c=1).
// ---------------------------------------------------------------------------

constexpr double kCam16LA = 64.0;   // _CAM16UCS_L_A
constexpr double kCam16Yb = 20.0;   // _CAM16UCS_Y_B
constexpr double kCam16SurroundF = 1.0;    // VIEWING_CONDITIONS "Average"
constexpr double kCam16SurroundC = 0.69;
constexpr double kCam16SurroundNc = 1.0;
constexpr double kUcsC1 = 0.007;    // COEFFICIENTS_UCS_LUO2006["CAM02-UCS"]
constexpr double kUcsC2 = 0.0228;
constexpr double kNpEps = 2.220446049250313e-16;  // np.finfo(float64).eps

constexpr double kCat16[3][3] = {  // colour.adaptation.CAT_CAT16 (MATRIX_16)
    {0.401288, 0.650173, -0.051461},
    {-0.250268, 1.204414, 0.045854},
    {-0.002079, 0.048952, 0.953127},
};
constexpr double kCat16Inv[3][3] = {  // np.linalg.inv(CAT_CAT16), exact hex
    {  0x1.dcb07a9c88540p+0, -0x1.02e1955e0fe8ep+0,  0x1.3188d60c3ca76p-3 },
    {  0x1.8cd3c2161fe30p-2,  0x1.3e2e5bee8e9d8p-1, -0x1.260f3e67a3c0bp-7 },
    { -0x1.038c0fde8f887p-6, -0x1.1788fcdc07728p-5,  0x1.0cca78265a79cp+0 },
};

// colour.algebra.sdiv under the default "Ignore Zero Conversion" mode: x/0 -> 0.
inline double sdiv(double a, double b) { return b == 0.0 ? 0.0 : a / b; }

// Everything about the viewing conditions + adapting white that is constant per
// output space (XYZ_to_CAM16's step 0). Built once per image.
struct Cam16Context {
    double D_RGB[3];
    double F_L;
    double n;
    double z;
    double N_bb;  // == N_cb
    double A_w;
    double Jp_white;  // _cam16ucs_white_Jp for the lightness knee
};

// colour.appearance: post-adaptation non-linear response compression (forward).
inline double cam16_pnrc_fwd(double v, double F_L) {
    const double f = spow(F_L * std::fabs(v) / 100.0, 0.42);
    return (400.0 * (v < 0.0 ? -1.0 : (v > 0.0 ? 1.0 : 0.0)) * f) / (27.13 + f) + 0.1;
}

// ...and its inverse (RGB here carries the +0.1 offset, exactly as in colour).
inline double cam16_pnrc_inv(double v, double F_L) {
    const double d = v - 0.1;
    const double sign = d < 0.0 ? -1.0 : (d > 0.0 ? 1.0 : 0.0);
    const double ratio = (27.13 * std::fabs(d)) / (400.0 - std::fabs(d));
    return sign * 100.0 / F_L * spow(ratio, 1.0 / 0.42);
}

// Forward CAM16 -> UCS J'a'b' for one unit-XYZ triple (the XYZ_to_UCS_Li2017 wrapper
// multiplies XYZ and XYZ_w by 100 under the reference domain-range scale).
void cam16ucs_from_xyz_unit(const double xyz_unit[3], int space, const Cam16Context& vc,
                            double jab[3]) {
    const double xyz[3] = {xyz_unit[0] * 100.0, xyz_unit[1] * 100.0,
                           xyz_unit[2] * 100.0};
    double rgb[3];
    mat3_mul(kCat16, xyz, rgb);
    double rgb_a[3];
    for (int k = 0; k < 3; ++k)
        rgb_a[k] = cam16_pnrc_fwd(vc.D_RGB[k] * rgb[k], vc.F_L);
    const double a = rgb_a[0] - 12.0 * rgb_a[1] / 11.0 + rgb_a[2] / 11.0;
    const double b = (rgb_a[0] + rgb_a[1] - 2.0 * rgb_a[2]) / 9.0;
    double h = std::atan2(b, a) * (180.0 / kPi);  // np.degrees(...) % 360
    h = std::fmod(h, 360.0);
    if (h < 0.0) h += 360.0;
    const double e_t = 0.25 * (std::cos(2.0 + h * kPi / 180.0) + 3.8);
    const double A =
        (2.0 * rgb_a[0] + rgb_a[1] + rgb_a[2] / 20.0 - 0.305) * vc.N_bb;
    const double J = 100.0 * spow(sdiv(A, vc.A_w), kCam16SurroundC * vc.z);
    const double t = (50000.0 / 13.0) * kCam16SurroundNc * vc.N_bb *
                     sdiv(e_t * std::sqrt(a * a + b * b),
                          rgb_a[0] + rgb_a[1] + 21.0 * rgb_a[2] / 20.0);
    const double C = spow(t, 0.9) * spow(J / 100.0, 0.5) *
                     std::pow(1.64 - std::pow(0.29, vc.n), 0.73);
    const double M = C * spow(vc.F_L, 0.25);
    // JMh -> CAM16-UCS (Li 2017): J' = (1+100 c1) J / (1 + c1 J); M' = log1p(c2 M)/c2.
    const double Jp = ((1.0 + 100.0 * kUcsC1) * J) / (1.0 + kUcsC1 * J);
    const double Mp = std::log1p(kUcsC2 * M) / kUcsC2;
    const double hr = h * (kPi / 180.0);
    jab[0] = Jp;
    jab[1] = Mp * std::cos(hr);
    jab[2] = Mp * std::sin(hr);
}

// Inverse: UCS J'a'b' -> unit XYZ (the UCS_Li2017_to_XYZ wrapper divides by 100).
void xyz_unit_from_cam16ucs(const double jab[3], int space, const Cam16Context& vc,
                            double xyz_unit[3]) {
    const double Jp = jab[0];
    const double J = -Jp / (kUcsC1 * Jp - 1.0 - 100.0 * kUcsC1);
    const double Mp = std::hypot(jab[2], jab[1]);
    double h = std::atan2(jab[2], jab[1]) * (180.0 / kPi);
    h = std::fmod(h, 360.0);
    if (h < 0.0) h += 360.0;
    const double M = std::expm1(Mp * kUcsC2) / kUcsC2;
    const double C = M / spow(vc.F_L, 0.25);
    // temporary_magnitude_quantity_inverse (J_prime = max(J, np.finfo eps)).
    const double J_prime = std::fmax(J, kNpEps);
    const double t = spow(
        C / (std::sqrt(J_prime / 100.0) * std::pow(1.64 - std::pow(0.29, vc.n), 0.73)),
        1.0 / 0.9);
    const double e_t = 0.25 * (std::cos(2.0 + h * kPi / 180.0) + 3.8);
    const double A = vc.A_w * spow(J / 100.0, 1.0 / (kCam16SurroundC * vc.z));
    const double P_1 = sdiv((50000.0 / 13.0) * kCam16SurroundNc * vc.N_bb * e_t, t);
    const double P_2 = A / vc.N_bb + 0.305;
    const double P_3 = 21.0 / 20.0;
    const double hr = h * (kPi / 180.0);
    const double sin_hr = std::sin(hr);
    const double cos_hr = std::cos(hr);
    const double n_val = P_2 * (2.0 + P_3) * (460.0 / 1403.0);
    double a = 0.0;
    double b = 0.0;
    if (std::fabs(sin_hr) >= std::fabs(cos_hr)) {
        const double P_4 = sdiv(P_1, sin_hr);
        b = n_val / (P_4 + (2.0 + P_3) * (220.0 / 1403.0) * sdiv(cos_hr, sin_hr) -
                     (27.0 / 1403.0) + P_3 * (6300.0 / 1403.0));
        a = b * sdiv(cos_hr, sin_hr);
    } else {
        const double P_5 = sdiv(P_1, cos_hr);
        a = n_val / (P_5 + (2.0 + P_3) * (220.0 / 1403.0) -
                     ((27.0 / 1403.0) - P_3 * (6300.0 / 1403.0)) * sdiv(sin_hr, cos_hr));
        b = a * sdiv(sin_hr, cos_hr);
    }
    if (t == 0.0) { a = 0.0; b = 0.0; }
    const double rgb_a[3] = {
        (460.0 * P_2 + 451.0 * a + 288.0 * b) / 1403.0,
        (460.0 * P_2 - 891.0 * a - 261.0 * b) / 1403.0,
        (460.0 * P_2 - 220.0 * a - 6300.0 * b) / 1403.0,
    };
    double rgb[3];
    for (int k = 0; k < 3; ++k)
        rgb[k] = cam16_pnrc_inv(rgb_a[k], vc.F_L) / vc.D_RGB[k];
    double xyz[3];
    mat3_mul(kCat16Inv, rgb, xyz);
    xyz_unit[0] = xyz[0] / 100.0;
    xyz_unit[1] = xyz[1] / 100.0;
    xyz_unit[2] = xyz[2] / 100.0;
}

// Step-0 context for `space` (XYZ_w = whitepoint at Y=1, x100 by the wrapper).
Cam16Context build_cam16_context(int space) {
    Cam16Context vc;
    const double xyz_w[3] = {kWhitepointXyzUnitY[space][0] * 100.0,
                             kWhitepointXyzUnitY[space][1] * 100.0,
                             kWhitepointXyzUnitY[space][2] * 100.0};
    const double Y_w = xyz_w[1];
    double rgb_w[3];
    mat3_mul(kCat16, xyz_w, rgb_w);
    // degree_of_adaptation clipped to [0, 1] (discount_illuminant = False).
    double D = kCam16SurroundF *
               (1.0 - (1.0 / 3.6) * std::exp((-kCam16LA - 42.0) / 92.0));
    if (D < 0.0) D = 0.0;
    else if (D > 1.0) D = 1.0;
    // viewing_conditions_dependent_parameters.
    vc.n = kCam16Yb / Y_w;
    const double k = 1.0 / (5.0 * kCam16LA + 1.0);
    const double k4 = k * k * k * k;
    vc.F_L = 0.2 * k4 * (5.0 * kCam16LA) +
             0.1 * (1.0 - k4) * (1.0 - k4) * spow(5.0 * kCam16LA, 1.0 / 3.0);
    vc.N_bb = 0.725 * spow(1.0 / vc.n, 0.2);
    vc.z = 1.48 + std::sqrt(vc.n);
    double rgb_aw[3];
    for (int c = 0; c < 3; ++c) {
        vc.D_RGB[c] = D * Y_w / rgb_w[c] + 1.0 - D;
        rgb_aw[c] = cam16_pnrc_fwd(vc.D_RGB[c] * rgb_w[c], vc.F_L);
    }
    vc.A_w = (2.0 * rgb_aw[0] + rgb_aw[1] + rgb_aw[2] / 20.0 - 0.305) * vc.N_bb;
    // _cam16ucs_white_Jp: the whitepoint through the very same forward path.
    vc.Jp_white = 0.0;
    double jab_w[3];
    cam16ucs_from_xyz_unit(kWhitepointXyzUnitY[space], space, vc, jab_w);
    vc.Jp_white = jab_w[0];
    return vc;
}

// _get_output_c_max_table("cam16ucs", ...): Jp grid linspace(1, 110, 64) (endpoint
// pinned), chroma upper 150, same 720-hue/18-bisection geometry.
void build_cam16ucs_cmax_table(int space, const Cam16Context& vc, double* table,
                               double* L_grid, double* h_grid) {
    const double Lstart = 1.0;
    const double Lend = 110.0;
    const double Lstep = (Lend - Lstart) / static_cast<double>(kOklchNL - 1);
    for (int i = 0; i < kOklchNL; ++i)
        L_grid[i] = static_cast<double>(i) * Lstep + Lstart;
    L_grid[kOklchNL - 1] = Lend;  // np.linspace endpoint override
    const double hstart = -kPi;
    const double hstep = (kPi - (-kPi)) / static_cast<double>(kOklchNH);
    for (int j = 0; j < kOklchNH; ++j)
        h_grid[j] = static_cast<double>(j) * hstep + hstart;

    const double(*M)[3] = kXyzToRgb[space];
    for (int i = 0; i < kOklchNL; ++i) {
        const double Jp = L_grid[i];
        for (int j = 0; j < kOklchNH; ++j) {
            const double ch = std::cos(h_grid[j]);
            const double sh = std::sin(h_grid[j]);
            double lo = 0.0;
            double hi = 150.0;
            for (int it = 0; it < kOklchNBisect; ++it) {
                const double mid = (lo + hi) * 0.5;
                const double jab[3] = {Jp, mid * ch, mid * sh};
                double xyz_unit[3];
                xyz_unit_from_cam16ucs(jab, space, vc, xyz_unit);
                double rgb[3];
                mat3_mul(M, xyz_unit, rgb);
                const bool in_gamut =
                    rgb[0] >= -1e-6 && rgb[0] <= 1.0 + 1e-6 &&
                    rgb[1] >= -1e-6 && rgb[1] <= 1.0 + 1e-6 &&
                    rgb[2] >= -1e-6 && rgb[2] <= 1.0 + 1e-6;
                if (in_gamut) lo = mid; else hi = mid;
            }
            table[static_cast<size_t>(i) * kOklchNH + j] = lo;
        }
    }
}

// compress_rgb_cam16ucs_chroma per pixel (lightness_compression pinned (0.7,1.0,2.2),
// L_white = the whitepoint's Jp). `out` may alias `rgb_in`.
void compress_pixel_cam16ucs(const double rgb_in[3], int space, const Cam16Context& vc,
                             const double* table, const double* L_grid,
                             const double* h_grid, double threshold, double limit,
                             double power, double out[3]) {
    double xyz_unit[3];
    mat3_mul(kRgbToXyz[space], rgb_in, xyz_unit);
    double jab[3];
    cam16ucs_from_xyz_unit(xyz_unit, space, vc, jab);
    const double ap = jab[1];
    const double bp = jab[2];

    const double Jp =
        reinhard_knee(jab[0] / vc.Jp_white, 0.7, 1.0, 2.2) * vc.Jp_white;
    const double Cp = std::hypot(ap, bp);
    const double hp = std::atan2(bp, ap);

    const double Cp_max = cmax_lookup(Jp, hp, table, L_grid, h_grid);
    const double safe_Cp_max = std::fmax(Cp_max, 1e-9);
    const double d_norm = Cp / safe_Cp_max;
    const double d_comp = reinhard_knee(d_norm, threshold, limit, power);
    const double Cp_new = d_comp * safe_Cp_max;

    const double jab_new[3] = {Jp, Cp_new * std::cos(hp), Cp_new * std::sin(hp)};
    double xyz_new[3];
    xyz_unit_from_cam16ucs(jab_new, space, vc, xyz_new);
    mat3_mul(kXyzToRgb[space], xyz_new, out);
}

}  // namespace

void compress_rgb_oklch_chroma(double* rgb, int npix, int output_space, double threshold,
                               double limit, double power) {
    // Build the per-space C_max(L,h) table ONCE, locally (no static/global state ->
    // thread-invariant and warm==cold). One build per image; this path is opt-in.
    std::vector<double> table(static_cast<size_t>(kOklchNL) * kOklchNH);
    double L_grid[kOklchNL];
    double h_grid[kOklchNH];
    build_oklch_cmax_table(output_space, table.data(), L_grid, h_grid);
    for (int p = 0; p < npix; ++p) {
        double* px = rgb + static_cast<long>(p) * 3;
        const double in[3] = {px[0], px[1], px[2]};
        compress_pixel_oklch(in, output_space, table.data(), L_grid, h_grid, threshold,
                             limit, power, px);
    }
}

void compress_rgb_oklrab_chroma(double* rgb, int npix, int output_space, double threshold,
                                double limit, double power) {
    // Build the per-space C_max(Lr,h) table ONCE, locally (no static/global state ->
    // thread-invariant and warm==cold). One build per image; this path is opt-in.
    std::vector<double> table(static_cast<size_t>(kOklchNL) * kOklchNH);
    double L_grid[kOklchNL];
    double h_grid[kOklchNH];
    build_oklrab_cmax_table(output_space, table.data(), L_grid, h_grid);
    for (int p = 0; p < npix; ++p) {
        double* px = rgb + static_cast<long>(p) * 3;
        const double in[3] = {px[0], px[1], px[2]};
        compress_pixel_oklrab(in, output_space, table.data(), L_grid, h_grid, threshold,
                              limit, power, px);
    }
}

void compress_rgb_jzazbz_chroma(double* rgb, int npix, int output_space,
                                double threshold, double limit, double power) {
    // Build the per-space Cz_max(Jz,hz) table ONCE, locally (no static/global state ->
    // thread-invariant and warm==cold). One build per image; this path is opt-in.
    std::vector<double> table(static_cast<size_t>(kOklchNL) * kOklchNH);
    double L_grid[kOklchNL];
    double h_grid[kOklchNH];
    build_jzazbz_cmax_table(output_space, table.data(), L_grid, h_grid);
    const double Jz_white = jzazbz_white_Jz(output_space);
    for (int p = 0; p < npix; ++p) {
        double* px = rgb + static_cast<long>(p) * 3;
        const double in[3] = {px[0], px[1], px[2]};
        compress_pixel_jzazbz(in, output_space, table.data(), L_grid, h_grid, Jz_white,
                              threshold, limit, power, px);
    }
}

void compress_rgb_cam16ucs_chroma(double* rgb, int npix, int output_space,
                                  double threshold, double limit, double power) {
    // Build the viewing-conditions context + per-space Cp_max(Jp,hp) table ONCE,
    // locally (no static/global state -> thread-invariant and warm==cold). The table
    // build runs ~830k CAM16 inversions and is the dominant cost; this path is opt-in.
    const Cam16Context vc = build_cam16_context(output_space);
    std::vector<double> table(static_cast<size_t>(kOklchNL) * kOklchNH);
    double L_grid[kOklchNL];
    double h_grid[kOklchNH];
    build_cam16ucs_cmax_table(output_space, vc, table.data(), L_grid, h_grid);
    for (int p = 0; p < npix; ++p) {
        double* px = rgb + static_cast<long>(p) * 3;
        const double in[3] = {px[0], px[1], px[2]};
        compress_pixel_cam16ucs(in, output_space, vc, table.data(), L_grid, h_grid,
                                threshold, limit, power, px);
    }
}

}  // namespace spk

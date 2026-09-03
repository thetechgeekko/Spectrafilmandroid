/*
 * Spektrafilm for Android — native engine: scan color output (XYZ->RGB + CCTF).
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm.
 */
#include "color_output.h"

#include <cmath>
#include <stdexcept>
#include <string>

#include "spektra.h"

namespace spk {

// standard_illuminant("D50"), SpectralShape(380,780,5), normalised to mean 1.
// Extracted from colour-science via the parity oracle.
const float kIlluminantD50[81] = {
    2.850090653e-01f, 3.163288707e-01f, 3.476603148e-01f, 4.607654314e-01f, 5.738821868e-01f,
    6.158048695e-01f, 6.577391909e-01f, 6.782233445e-01f, 6.987191369e-01f, 6.858234311e-01f,
    6.729277253e-01f, 7.718917926e-01f, 8.708674987e-01f, 9.431556115e-01f, 1.015443724e+00f,
    1.035031696e+00f, 1.054608029e+00f, 1.059007467e+00f, 1.063406904e+00f, 1.085171318e+00f,
    1.106947370e+00f, 1.088639657e+00f, 1.070331945e+00f, 1.092212746e+00f, 1.114105185e+00f,
    1.119284417e+00f, 1.124452010e+00f, 1.127454801e+00f, 1.130457592e+00f, 1.159379820e+00f,
    1.188302048e+00f, 1.180480826e+00f, 1.172659604e+00f, 1.181749447e+00f, 1.190839290e+00f,
    1.177361648e+00f, 1.163872367e+00f, 1.150697332e+00f, 1.137510658e+00f, 1.144400782e+00f,
    1.151279268e+00f, 1.119738327e+00f, 1.088209025e+00f, 1.112580512e+00f, 1.136963638e+00f,
    1.146158230e+00f, 1.155364460e+00f, 1.154037646e+00f, 1.152722470e+00f, 1.133402189e+00f,
    1.114081907e+00f, 1.132331426e+00f, 1.150569306e+00f, 1.132005542e+00f, 1.113441778e+00f,
    1.128129847e+00f, 1.142806277e+00f, 1.170820685e+00f, 1.198823454e+00f, 1.176302524e+00f,
    1.153781594e+00f, 1.085392453e+00f, 1.017003313e+00f, 1.041572659e+00f, 1.066153643e+00f,
    1.073625704e+00f, 1.081109403e+00f, 9.878017555e-01f, 8.944824691e-01f, 9.506858657e-01f,
    1.006877624e+00f, 1.042201150e+00f, 1.077513038e+00f, 9.940051952e-01f, 9.104973528e-01f,
    7.909792995e-01f, 6.714612461e-01f, 8.182837452e-01f, 9.651178830e-01f, 9.380694892e-01f,
    9.110094567e-01f,
};

// sum(illuminant * ybar) over the working shape (the XYZ denominator).
const double kNormD50 = 24.451263183234168;

// standard_illuminant("K75P") -> colour SDS_LIGHT_SOURCES["Kinoton 75P"],
// SpectralShape(380,780,5), normalized to mean 1. Extracted from the pinned
// c1d0e44 oracle with colour-science 0.4.7.
const float kIlluminantK75P[81] = {
    2.003553510e-01f, 3.026585281e-01f, 4.125533104e-01f, 5.589481592e-01f, 6.999984980e-01f,
    8.502194285e-01f, 9.967964292e-01f, 1.103016973e+00f, 1.148383856e+00f, 1.166451573e+00f,
    1.217982888e+00f, 1.306287289e+00f, 1.415362239e+00f, 1.476124763e+00f, 1.581829071e+00f,
    1.637459755e+00f, 1.741797447e+00f, 1.919135332e+00f, 1.996872425e+00f, 1.818016291e+00f,
    1.703293324e+00f, 1.714619756e+00f, 1.665760875e+00f, 1.679213047e+00f, 1.599137664e+00f,
    1.568923473e+00f, 1.550551891e+00f, 1.537160516e+00f, 1.531421304e+00f, 1.520914674e+00f,
    1.520914793e+00f, 1.510681391e+00f, 1.510286570e+00f, 1.501693010e+00f, 1.486175895e+00f,
    1.473999143e+00f, 1.465162635e+00f, 1.462004542e+00f, 1.478948712e+00f, 1.483959198e+00f,
    1.479130983e+00f, 1.471721768e+00f, 1.435768247e+00f, 1.385360599e+00f, 1.338779092e+00f,
    1.322563648e+00f, 1.322927952e+00f, 1.341663837e+00f, 1.348678350e+00f, 1.326784492e+00f,
    1.275982022e+00f, 1.203164220e+00f, 1.124758959e+00f, 1.082459211e+00f, 1.067731500e+00f,
    1.041282654e+00f, 1.049633384e+00f, 1.049542308e+00f, 9.857431650e-01f, 8.214324117e-01f,
    6.165525913e-01f, 4.288598597e-01f, 2.650957406e-01f, 1.552313268e-01f, 8.982282132e-02f,
    6.249336153e-02f, 4.571307451e-02f, 3.847076744e-02f, 2.714726143e-02f, 2.576256916e-02f,
    2.350333333e-02f, 2.138530090e-02f, 2.095258422e-02f, 2.241015621e-02f, 2.179068886e-02f,
    1.802833192e-02f, 1.590574533e-02f, 2.681019902e-02f, 2.732945792e-02f, 3.086406924e-02f,
    2.568969131e-02f,
};

const double kNormK75P = 31.24235608016079;

// colour.XYZ_to_RGB(eye, 'sRGB', illuminant=D50_xy) effective matrix, row-major.
// rgb = M . xyz. CAT02 D50->D65 adaptation + sRGB primaries baked in.
const float kXYZ_to_sRGB_D50[9] = {
     3.142560531830132e+00f, -1.632613796187014e+00f, -4.818555764518727e-01f,
    -9.697409102368959e-01f,  1.902220529621061e+00f,  3.987257349273618e-02f,
     5.923885096274457e-02f, -2.007043145336758e-01f,  1.386049599349437e+00f,
};

// matrix_RGB_to_RGB(sRGB, sRGB, "CAT02"), row-major. Extracted from
// colour-science via the parity oracle (near-identity, ~1e-5 off-diagonal).
const double kSRGB_to_sRGB_CAT02[9] = {
     0.99999174000000013,    1.1823931167498358e-16, 2.3159999999960313e-05,
     2.1670000000040608e-05, 1.0000403199999999,    -7.9399999999762392e-06,
     3.8000000000042523e-07, 1.1920000000013188e-05, 1.00000355,
};

float srgb_cctf_encode(float linear) {
    // colour: spow(L, 1/2.4) == sign(L)*pow(|L|, 1/2.4); inputs here are >= 0.
    if (linear <= 0.0031308f) return 12.92f * linear;
    return 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

// --- Per-output-space matrices ----------------------------------------------
// Effective XYZ -> output-RGB (linear) matrices. row-major (rgb = M . xyz).
// colour.XYZ_to_RGB(eye, colourspace, illuminant=D50_xy): CAT02 from the D50
// scan whitepoint to the space whitepoint + space primaries baked in. Extracted
// from colour-science via the parity oracle (bit-for-bit). Indexed by
// spk_color_space; LINEAR_SRGB (5) shares sRGB's matrix.
const double kXYZ_to_RGB[6][9] = {
    {  // [0] SRGB
        3.142560531830132,
        -1.6326137961870142,
        -0.48185557645187271,
        -0.9697409102368959,
        1.9022205296210604,
        0.039872573492736187,
        0.059238850962744567,
        -0.20070431453367582,
        1.3860495993494377
    },
    {  // [1] ADOBE_RGB
        1.9712349874153061,
        -0.62572112138316549,
        -0.33320749091235091,
        -0.97007079823624576,
        1.9024005644289221,
        0.039933450041275319,
        0.016801292045390288,
        -0.11411011568389857,
        1.3305940932494869
    },
    {  // [2] PROPHOTO
        1.3460511557389605,
        -0.25549887834774693,
        -0.05112002291427066,
        -0.5445537190485934,
        1.5081028885618692,
        0.020497944497017227,
        -1.0509873740792862e-06,
        6.8692002868351891e-08,
        1.2122747140648567
    },
    {  // [3] REC2020
        1.6550138362531475,
        -0.40669447721540475,
        -0.22913524712033548,
        -0.67418291166695654,
        1.6342291843689367,
        0.019167894711971223,
        0.019130981893892087,
        -0.039055210216034392,
        1.2369182318017684
    },
    {  // [4] ACES2065_1
        1.0195716907143211,
        -0.022810203261674832,
        0.048165423762075193,
        -0.50306460039388901,
        1.3844084125716802,
        0.12197568835784825,
        0.00096183477783376686,
        0.0030561904324193151,
        1.207113247346848
    },
    {  // [5] LINEAR_SRGB (== SRGB)
        3.142560531830132,
        -1.6326137961870142,
        -0.48185557645187271,
        -0.9697409102368959,
        1.9022205296210604,
        0.039872573492736187,
        0.059238850962744567,
        -0.20070431453367582,
        1.3860495993494377
    },
};

// colour.XYZ_to_RGB(..., illuminant=K75P_xy), where K75P_xy is the chromaticity
// of the same normalized Kinoton 75P SPD. CAT02 + output primaries are baked in,
// exactly as for the D50 table above. Indexed by spk_color_space.
const double kXYZ_to_RGB_K75P[6][9] = {
    {  // [0] SRGB
        3.232236500809162, -1.5349109384947712, -0.49798421014519223,
        -0.9667543480014241, 1.8719375025543321, 0.04139730241485097,
        0.055708159972140604, -0.20309720958383254, 1.0881780404577015
    },
    {  // [1] ADOBE_RGB
        2.036219858460815, -0.5644732114888867, -0.3443178249335176,
        -0.967093471845468, 1.8721072465127848, 0.04145748574692734,
        0.013536361703599623, -0.11765341732215541, 1.0450591469911041
    },
    {  // [2] PROPHOTO
        1.393986409970918, -0.2142761265310886, -0.10005299419877167,
        -0.5332668351636909, 1.4909560841243228, 0.01329440105882937,
        -0.0013131091840827228, -0.0037290267141805605, 0.9508668289291261
    },
    {  // [3] REC2020
        1.7121104808394825, -0.35546726732876277, -0.25166249001372737,
        -0.6652883893205055, 1.613097728625932, 0.01607100894694128,
        0.01769914428783694, -0.04226456558667562, 0.970026030975556
    },
    {  // [4] ACES2065_1
        1.0595148542431525, 0.008089687979870253, -0.010920824077060002,
        -0.49294121653555606, 1.3682539960189533, 0.09327139857999221,
        -0.00023965011624595432, -0.0006287366848202698, 0.9467131181608147
    },
    {  // [5] LINEAR_SRGB (== SRGB)
        3.232236500809162, -1.5349109384947712, -0.49798421014519223,
        -0.9667543480014241, 1.8719375025543321, 0.04139730241485097,
        0.055708159972140604, -0.20309720958383254, 1.0881780404577015
    },
};

const ViewingIlluminant* find_viewing_illuminant(
    std::string_view identifier) noexcept {
    static const ViewingIlluminant kD50{
        "D50", kIlluminantD50, kNormD50, kXYZ_to_RGB};
    static const ViewingIlluminant kK75P{
        "K75P", kIlluminantK75P, kNormK75P, kXYZ_to_RGB_K75P};
    if (identifier == kD50.identifier) return &kD50;
    if (identifier == kK75P.identifier) return &kK75P;
    return nullptr;
}

const ViewingIlluminant& require_viewing_illuminant(
    std::string_view identifier) {
    if (const ViewingIlluminant* illuminant =
            find_viewing_illuminant(identifier)) {
        return *illuminant;
    }
    throw std::runtime_error(
        "Profile: unsupported viewing_illuminant '" + std::string(identifier) +
        "' (expected exact identifier D50 or K75P)");
}

// Near-identity RGB->RGB matrices colour applies (matrix_RGB_to_RGB(cs, cs,
// "CAT02")) inside _apply_cctf_encoding_and_clip, before the CCTF encode, when
// output_cctf_encoding is on. row-major (rgb_out = M . rgb_in). Indexed by
// spk_color_space.
const double kRGB_to_RGB_CCTF[6][9] = {
    {  // [0] SRGB
        0.99999174000000013,
        1.1823931167498358e-16,
        2.3159999999960313e-05,
        2.1670000000040608e-05,
        1.0000403199999999,
        -7.9399999999762392e-06,
        3.8000000000042523e-07,
        1.1920000000013188e-05,
        1.00000355
    },
    {  // [1] ADOBE_RGB
        1.0000055799999998,
        3.8030999998693774e-06,
        4.24459999995054e-06,
        -7.3442000000058817e-06,
        0.99999424120000002,
        1.8265000000026845e-06,
        -2.672500000003587e-06,
        1.9640999999931614e-06,
        0.99999711459999996
    },
    {  // [2] PROPHOTO
        1.0000914000000001,
        1.7559999999962318e-05,
        -4.8150000000010725e-05,
        -6.5819999999990205e-05,
        1.00005766,
        1.5290000000007618e-05,
        -1.3908398461908093e-18,
        -7.8277489333936824e-19,
        1.00002627
    },
    {  // [3] REC2020
        1,
        -3.2317645347557231e-17,
        6.2041613010914368e-18,
        -2.3542968446340584e-17,
        1,
        8.0487377619960405e-18,
        8.0943508849239081e-19,
        -9.6915099471300284e-19,
        0.99999999999999989
    },
    {  // [4] ACES2065_1
        0.99999999996184175,
        -2.2362308506423466e-17,
        7.6953142761556508e-12,
        3.9892802275237737e-11,
        0.99999999997004307,
        3.5211567385746431e-11,
        -1.3580416246962525e-18,
        -4.9119815365090854e-19,
        1.0000000000474871
    },
    {  // [5] LINEAR_SRGB (== SRGB; unused: CCTF is off for this space)
        0.99999174000000013,
        1.1823931167498358e-16,
        2.3159999999960313e-05,
        2.1670000000040608e-05,
        1.0000403199999999,
        -7.9399999999762392e-06,
        3.8000000000042523e-07,
        1.1920000000013188e-05,
        1.00000355
    },
};

namespace {
// Sign-preserving power: spow(L, e) = sign(L) * pow(|L|, e). Matches colour.spow.
inline double spow(double L, double e) {
    if (L == 0.0) return 0.0;
    return std::copysign(std::pow(std::fabs(L), e), L);
}
}  // namespace

double output_cctf_encode(spk_color_space cs, double L) {
    switch (cs) {
        case SPK_CS_ADOBE_RGB:
            // colour gamma_function(L, 0.4547069271758437) with "Indeterminate"
            // negative handling: plain L**exp -> NaN for L < 0 (NOT sign-preserving).
            return std::pow(L, 0.4547069271758437);
        case SPK_CS_PROPHOTO: {
            // cctf_encoding_ROMMRGB: E_t = 16^(1.8/(1-1.8)); below E_t -> 16*L,
            // else spow(L, 1/1.8). (I_max factors cancel in the float-1 path.)
            const double E_t = 0.001953125;  // 16 ** (1.8/(1-1.8))
            return (E_t > L) ? (L * 16.0) : spow(L, 1.0 / 1.8);
        }
        case SPK_CS_REC2020: {
            // oetf_BT2020 (non-12-bit): a=1.099, b=0.018; below b -> 4.5*L,
            // else a*spow(L,0.45) - (a-1).
            const double a = 1.099, b = 0.018;
            return (b > L) ? (L * 4.5) : (a * spow(L, 0.45) - (a - 1.0));
        }
        case SPK_CS_ACES2065_1:
            // linear_function: identity.
            return L;
        case SPK_CS_SRGB:
        case SPK_CS_LINEAR_SRGB:
        default:
            // eotf_inverse_sRGB: piecewise; spow(L,1/2.4) sign-preserving.
            return (L <= 0.0031308) ? (12.92 * L)
                                    : (1.055 * spow(L, 1.0 / 2.4) - 0.055);
    }
}

}  // namespace spk

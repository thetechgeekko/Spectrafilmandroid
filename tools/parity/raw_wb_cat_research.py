#!/usr/bin/env python3
"""Reproduce the RAW white-balance CAT decision without production code.

The pinned Spektrafilm oracle calls ``colour.chromatic_adaptation`` with
``method="Von Kries"`` and no explicit ``transform``.  In colour-science 0.4.7
that resolves to CAT02.  This research-only program evaluates that oracle
against the Android decoder's current XYZ-scaling approximation, Bradford, and
no adaptation.  It deliberately uses only the Python standard library so the
committed decision remains checkable in offline CI.

Nothing in this file is called by the Android application or native decoder.
The host/native golden build can ask it to emit a C++ fixture header; that path
first verifies the immutable JSON digest, so generated expectations cannot drift
with production code or be rebaselined by accident.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import platform
import struct
import sys
from typing import Any


ORACLE_CANDIDATE = "cat02"
CANDIDATES = (
    ORACLE_CANDIDATE,
    "android_xyz_scaling",
    "bradford",
    "no_adaptation",
    "cat02_single_round_tint",
    "cat02_no_reference_skip",
    "cat02_no_tint_skip",
)

AP0_TO_XYZ = (
    (0.9525523959, 0.0, 0.0000936786),
    (0.3439664498, 0.7281660966, -0.0721325464),
    (0.0, 0.0, 1.0088251844),
)
XYZ_TO_AP0 = (
    (1.0498110175, 0.0, -0.0000974845),
    (-0.4959030231, 1.3733130458, 0.0982400361),
    (0.0, 0.0, 0.9912520182),
)
CAT02 = (
    (0.7328, 0.4296, -0.1624),
    (-0.7036, 1.6975, 0.0061),
    (0.0030, 0.0136, 0.9834),
)
BRADFORD = (
    (0.8951, 0.2664, -0.1614),
    (-0.7502, 1.7135, 0.0367),
    (0.0389, -0.0685, 1.0296),
)
ACES_TO_PROPHOTO = (
    (1.2393803417847302, -0.16396782280140051, -0.07523338379836997),
    (0.003611361866381234, 1.0896136492217019, -0.09326579208197863),
    (-0.00205967931567552, -0.0022515883414713734, 1.0045855773288515),
)
DAYLIGHT_REFERENCE_K = 6504.0
TUNGSTEN_K = 2850.0

Matrix = tuple[tuple[float, float, float], ...]
Vector = tuple[float, float, float]


def _f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", float(value)))[0]


def _f32_vector(values: Vector | list[float]) -> Vector:
    return tuple(_f32(value) for value in values)  # type: ignore[return-value]


def _f32_bits(values: Vector) -> list[int]:
    return [struct.unpack("<I", struct.pack("<f", value))[0] for value in values]


def _mat_vec(matrix: Matrix, vector: Vector) -> Vector:
    return tuple(
        sum(matrix[row][column] * vector[column] for column in range(3))
        for row in range(3)
    )  # type: ignore[return-value]


def _mat_mul(lhs: Matrix, rhs: Matrix) -> Matrix:
    return tuple(
        tuple(
            sum(lhs[row][k] * rhs[k][column] for k in range(3))
            for column in range(3)
        )
        for row in range(3)
    )


def _inverse(matrix: Matrix) -> Matrix:
    a, b, c = matrix[0]
    d, e, f = matrix[1]
    g, h, i = matrix[2]
    determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    if determinant == 0.0:
        raise ValueError("singular 3x3 matrix")
    inverse_determinant = 1.0 / determinant
    return (
        ((e * i - f * h) * inverse_determinant,
         (c * h - b * i) * inverse_determinant,
         (b * f - c * e) * inverse_determinant),
        ((f * g - d * i) * inverse_determinant,
         (a * i - c * g) * inverse_determinant,
         (c * d - a * f) * inverse_determinant),
        ((d * h - e * g) * inverse_determinant,
         (b * g - a * h) * inverse_determinant,
         (a * e - b * d) * inverse_determinant),
    )


def _diagonal(values: Vector) -> Matrix:
    return (
        (values[0], 0.0, 0.0),
        (0.0, values[1], 0.0),
        (0.0, 0.0, values[2]),
    )


def _whitepoint_xyz(temperature_k: float) -> Vector:
    """Match colour 0.4.7 CIE-D/Kang-2002 xy -> XYZ(Y=1) math."""

    if not math.isfinite(temperature_k) or temperature_k <= 0.0:
        raise ValueError("temperature must be finite and positive")
    t = temperature_k
    if t >= 4000.0:
        if t <= 7000.0:
            x = -4.607e9 / t**3 + 2.9678e6 / t**2 + 0.09911e3 / t + 0.244063
        else:
            x = -2.0064e9 / t**3 + 1.9018e6 / t**2 + 0.24748e3 / t + 0.237040
        y = -3.0 * x**2 + 2.87 * x - 0.275
    else:
        x = -0.2661239e9 / t**3 - 0.2343589e6 / t**2 + 0.8776956e3 / t + 0.179910
        if t <= 2222.0:
            y = -1.1063814 * x**3 - 1.34811020 * x**2 + 2.18555832 * x - 0.20219683
        else:
            y = -0.9549476 * x**3 - 1.37418593 * x**2 + 2.09137015 * x - 0.16748867
    return (x / y, 1.0, (1.0 - x - y) / y)


def _allclose(lhs: Vector, rhs: Vector) -> bool:
    # numpy.allclose defaults used by the pinned oracle's _postprocess_params.
    return all(abs(a - b) <= 1.0e-8 + 1.0e-5 * abs(b) for a, b in zip(lhs, rhs))


def _isclose(lhs: float, rhs: float) -> bool:
    # numpy.isclose defaults used by _apply_tint_adjustment.
    return abs(lhs - rhs) <= 1.0e-8 + 1.0e-5 * abs(rhs)


def _cat_matrix(source_white: Vector, target_white: Vector, cone: Matrix) -> Matrix:
    source_cone = _mat_vec(cone, source_white)
    target_cone = _mat_vec(cone, target_white)
    ratios = tuple(target / source for source, target in zip(source_cone, target_cone))
    return _mat_mul(_mat_mul(_inverse(cone), _diagonal(ratios)), cone)  # type: ignore[arg-type]


def _scenario_adjustment(scenario: dict[str, Any]) -> tuple[bool, float, float]:
    mode = scenario["mode"]
    if mode in ("as_shot", "daylight"):
        return False, DAYLIGHT_REFERENCE_K, 1.0
    if mode == "tungsten":
        return True, TUNGSTEN_K, 1.0
    if mode == "custom":
        temperature_k = float(scenario["temperature_k"])
        tint = float(scenario["tint"])
        if not math.isfinite(tint):
            raise ValueError("tint must be finite")
        return True, temperature_k, tint
    raise ValueError(f"unsupported white-balance mode: {mode}")


def _adapt_aces(rgb: Vector, scenario: dict[str, Any], candidate: str) -> Vector:
    rgb = _f32_vector(rgb)
    adjust, temperature_k, tint = _scenario_adjustment(scenario)
    if not adjust:
        return rgb

    source_white = _whitepoint_xyz(temperature_k)
    target_white = _whitepoint_xyz(DAYLIGHT_REFERENCE_K)
    # Match upstream argument order exactly.  NumPy's rtol is applied to the
    # second operand, so allclose(reference, scene) is not interchangeable with
    # allclose(scene, reference) at the tolerance boundary.
    skip_adaptation = _allclose(target_white, source_white)
    # The skip belongs to the upstream oracle contract.  Production Android's
    # current XYZ-scaling approximation does not have it, which is itself a
    # measurable near-reference parity difference.
    candidate_skips = skip_adaptation and candidate not in (
        "android_xyz_scaling",
        "cat02_no_reference_skip",
    )
    tint_skips = _isclose(tint, 1.0) and candidate not in (
        "android_xyz_scaling",
        "cat02_no_tint_skip",
    )
    xyz = _mat_vec(AP0_TO_XYZ, rgb)

    if candidate_skips or candidate == "no_adaptation":
        adapted_double = rgb
    elif candidate == "android_xyz_scaling":
        scale = tuple(target / source for source, target in zip(source_white, target_white))
        adapted_double = _mat_vec(XYZ_TO_AP0, _mat_vec(_diagonal(scale), xyz))  # type: ignore[arg-type]
    else:
        cone = BRADFORD if candidate == "bradford" else CAT02
        adapted_double = _mat_vec(XYZ_TO_AP0, _mat_vec(_cat_matrix(source_white, target_white, cone), xyz))

    if candidate == "android_xyz_scaling" or (
        candidate == "cat02_single_round_tint" and not candidate_skips and not tint_skips
    ):
        # Current Android combines the green tint multiply with the final cast.
        return _f32_vector((adapted_double[0], adapted_double[1] * tint, adapted_double[2]))

    # The oracle casts CAT output to float32, then applies a float32 tint vector.
    adapted = _f32_vector(adapted_double)
    if tint_skips:
        return adapted
    tint32 = _f32(tint)
    return _f32_vector((adapted[0], adapted[1] * tint32, adapted[2]))


def _to_prophoto(rgb: Vector) -> Vector:
    # Android's engine boundary is float32, so the float64 colour conversion is
    # explicitly rounded here.  This is the golden boundary, not whole-array
    # float64 identity with the desktop Python return type.
    return _f32_vector(_mat_vec(ACES_TO_PROPHOTO, rgb))


def _metrics(candidate_values: list[float], oracle_values: list[float]) -> dict[str, float]:
    deltas = [candidate - oracle for candidate, oracle in zip(candidate_values, oracle_values)]
    return {
        "max_abs": max((abs(delta) for delta in deltas), default=0.0),
        "rms": math.sqrt(sum(delta * delta for delta in deltas) / max(1, len(deltas))),
    }


def _build_report(fixture: dict[str, Any]) -> dict[str, Any]:
    vectors: list[dict[str, Any]] = []
    aggregate: dict[str, dict[str, list[float]]] = {
        candidate: {"aces": [], "prophoto": [], "oracle_aces": [], "oracle_prophoto": []}
        for candidate in CANDIDATES
    }
    scenario_summary: list[dict[str, Any]] = []

    for scenario in fixture["scenarios"]:
        per_scenario: dict[str, dict[str, list[float]]] = {
            candidate: {"aces": [], "prophoto": [], "oracle_aces": [], "oracle_prophoto": []}
            for candidate in CANDIDATES
        }
        for patch in fixture["patches"]:
            input_rgb = _f32_vector(patch["aces_rgb"])
            oracle_aces = _adapt_aces(input_rgb, scenario, ORACLE_CANDIDATE)
            oracle_prophoto = _to_prophoto(oracle_aces)
            outputs: dict[str, Any] = {}
            for candidate in CANDIDATES:
                aces = _adapt_aces(input_rgb, scenario, candidate)
                prophoto = _to_prophoto(aces)
                outputs[candidate] = {
                    "aces_after_wb": list(aces),
                    "prophoto_float32": list(prophoto),
                    "aces_bits": _f32_bits(aces),
                    "prophoto_bits": _f32_bits(prophoto),
                }
                for bucket in (aggregate[candidate], per_scenario[candidate]):
                    bucket["aces"].extend(aces)
                    bucket["prophoto"].extend(prophoto)
                    bucket["oracle_aces"].extend(oracle_aces)
                    bucket["oracle_prophoto"].extend(oracle_prophoto)
            vectors.append({
                "scenario": scenario["id"],
                "patch": patch["id"],
                "input_aces_float32": list(input_rgb),
                "outputs": outputs,
            })

        scenario_summary.append({
            "scenario": scenario["id"],
            "metrics": {
                candidate: {
                    "aces": _metrics(values["aces"], values["oracle_aces"]),
                    "prophoto": _metrics(values["prophoto"], values["oracle_prophoto"]),
                }
                for candidate, values in per_scenario.items()
            },
        })

    overall = {
        candidate: {
            "aces": _metrics(values["aces"], values["oracle_aces"]),
            "prophoto": _metrics(values["prophoto"], values["oracle_prophoto"]),
        }
        for candidate, values in aggregate.items()
    }
    return {
        "schema_version": 1,
        "oracle": fixture["oracle"],
        "selected_model": fixture["selected_model"],
        "target_white": fixture["target_white"],
        "scenarios": fixture["scenarios"],
        "patches": fixture["patches"],
        "overall": overall,
        "scenario_summary": scenario_summary,
        "vectors": vectors,
    }


def _digest_payload(report: dict[str, Any]) -> dict[str, Any]:
    """Return the cross-Python lock surface for the research decision.

    Aggregate RMS values contain ``sqrt`` results whose final float64 bits can
    vary across Python/libm versions even when every declared float32 pixel is
    identical.  The immutable digest therefore covers provenance, inputs, and
    every candidate's float32 bit pattern; metrics remain derived report data.
    """

    return {
        "digest_schema_version": 1,
        "report_schema_version": report["schema_version"],
        "oracle": report["oracle"],
        "selected_model": report["selected_model"],
        "target_white": report["target_white"],
        "scenarios": report["scenarios"],
        "patches": report["patches"],
        "candidates": list(CANDIDATES),
        "vectors": [
            {
                "scenario": vector["scenario"],
                "patch": vector["patch"],
                "input_aces_bits": _f32_bits(
                    tuple(float(value) for value in vector["input_aces_float32"])
                ),
                "outputs": {
                    candidate: {
                        "aces_bits": output["aces_bits"],
                        "prophoto_bits": output["prophoto_bits"],
                    }
                    for candidate, output in vector["outputs"].items()
                },
            }
            for vector in report["vectors"]
        ],
    }


def _canonical_bytes(report: dict[str, Any]) -> bytes:
    return json.dumps(
        _digest_payload(report),
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    ).encode("utf-8")


def _report_digest(report: dict[str, Any]) -> str:
    return hashlib.sha256(_canonical_bytes(report)).hexdigest()


def _cpp_fixture_header(fixture: dict[str, Any], report: dict[str, Any]) -> str:
    """Generate the build-only C++ table consumed by the production-math test."""

    scenarios = {scenario["id"]: scenario for scenario in fixture["scenarios"]}
    mode_code = {"as_shot": 0, "daylight": 1, "tungsten": 2, "custom": 3}
    lines = [
        "// Generated by tools/parity/raw_wb_cat_research.py; do not edit.",
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "namespace sfraw::raw_wb_fixture {",
        "struct Vector {",
        "  const char* scenario;",
        "  const char* patch;",
        "  int mode;",
        "  double temperature_k;",
        "  double tint;",
        "  std::uint32_t input_aces_bits[3];",
        "  std::uint32_t expected_aces_bits[3];",
        "  std::uint32_t expected_prophoto_bits[3];",
        "  std::uint32_t wrong_single_round_tint_aces_bits[3];",
        "  std::uint32_t wrong_no_reference_skip_aces_bits[3];",
        "  std::uint32_t wrong_no_tint_skip_aces_bits[3];",
        "};",
        f'inline constexpr const char kReportSha256[] = "{_report_digest(report)}";',
        "inline constexpr Vector kVectors[] = {",
    ]
    for vector in report["vectors"]:
        scenario = scenarios[vector["scenario"]]
        mode = scenario["mode"]
        temperature_k = float(scenario.get("temperature_k", DAYLIGHT_REFERENCE_K))
        tint = float(scenario.get("tint", 1.0))
        output = vector["outputs"][ORACLE_CANDIDATE]
        wrong_single_round = vector["outputs"]["cat02_single_round_tint"]
        wrong_no_reference_skip = vector["outputs"]["cat02_no_reference_skip"]
        wrong_no_tint_skip = vector["outputs"]["cat02_no_tint_skip"]
        input_bits = _f32_bits(
            tuple(float(value) for value in vector["input_aces_float32"])
        )
        lines.extend([
            "  {",
            f'    "{vector["scenario"]}", "{vector["patch"]}", {mode_code[mode]},',
            f"    {temperature_k:.17g}, {tint:.17g},",
            "    {" + ", ".join(f"0x{value:08x}u" for value in input_bits) + "},",
            "    {" + ", ".join(
                f"0x{value:08x}u" for value in output["aces_bits"]
            ) + "},",
            "    {" + ", ".join(
                f"0x{value:08x}u" for value in output["prophoto_bits"]
            ) + "},",
            "    {" + ", ".join(
                f"0x{value:08x}u" for value in wrong_single_round["aces_bits"]
            ) + "},",
            "    {" + ", ".join(
                f"0x{value:08x}u" for value in wrong_no_reference_skip["aces_bits"]
            ) + "},",
            "    {" + ", ".join(
                f"0x{value:08x}u" for value in wrong_no_tint_skip["aces_bits"]
            ) + "},",
            "  },",
        ])
    lines.extend([
        "};",
        "inline constexpr std::size_t kVectorCount = sizeof(kVectors) / sizeof(kVectors[0]);",
        "}  // namespace sfraw::raw_wb_fixture",
        "",
    ])
    return "\n".join(lines)


def _find_vector(report: dict[str, Any], scenario: str, patch: str) -> dict[str, Any]:
    return next(
        vector for vector in report["vectors"]
        if vector["scenario"] == scenario and vector["patch"] == patch
    )


def _verify_reference_bits(fixture: dict[str, Any], report: dict[str, Any]) -> None:
    for reference in fixture["colour_reference_vectors"]:
        vector = _find_vector(report, reference["scenario"], reference["patch"])
        actual = vector["outputs"][ORACLE_CANDIDATE]
        if actual["aces_bits"] != reference["aces_bits"]:
            raise AssertionError(
                f"colour reference ACES bits changed for {reference['scenario']}/{reference['patch']}"
            )
        if actual["prophoto_bits"] != reference["prophoto_bits"]:
            raise AssertionError(
                f"colour reference ProPhoto bits changed for {reference['scenario']}/{reference['patch']}"
            )


def _verify_with_colour(fixture: dict[str, Any], report: dict[str, Any]) -> None:
    try:
        import colour  # type: ignore[import-not-found]
        import numpy as np  # type: ignore[import-not-found]
    except ImportError as error:
        raise RuntimeError("--verify-colour requires colour-science and numpy") from error

    expected_environment = fixture["oracle"]["environment"]
    if colour.__version__ != expected_environment["colour_science"]:
        raise AssertionError(
            f"colour-science {colour.__version__} != pinned {expected_environment['colour_science']}"
        )
    if np.__version__ != expected_environment["numpy"]:
        raise AssertionError(f"numpy {np.__version__} != pinned {expected_environment['numpy']}")
    if platform.python_version() != expected_environment["python"]:
        raise AssertionError(
            f"Python {platform.python_version()} != pinned {expected_environment['python']}"
        )

    aces_space = colour.RGB_COLOURSPACES["ACES2065-1"]
    prophoto_space = colour.RGB_COLOURSPACES["ProPhoto RGB"]
    for scenario in fixture["scenarios"]:
        adjust, temperature_k, tint = _scenario_adjustment(scenario)
        for patch in fixture["patches"]:
            rgb = np.asarray(patch["aces_rgb"], dtype=np.float32)
            if adjust:
                source_method = (
                    "CIE Illuminant D Series" if temperature_k >= 4000.0 else "Kang 2002"
                )
                source_white = colour.xy_to_XYZ(
                    colour.CCT_to_xy(np.float64(temperature_k), method=source_method)
                )
                target_white = colour.xy_to_XYZ(
                    colour.CCT_to_xy(
                        np.float64(DAYLIGHT_REFERENCE_K),
                        method="CIE Illuminant D Series",
                    )
                )
                if not np.allclose(target_white, source_white):
                    xyz = colour.RGB_to_XYZ(
                        rgb,
                        colourspace=aces_space,
                        chromatic_adaptation_transform=None,
                        apply_cctf_decoding=False,
                    )
                    xyz = colour.chromatic_adaptation(
                        xyz, source_white, target_white, method="Von Kries"
                    )
                    rgb = colour.XYZ_to_RGB(
                        xyz,
                        colourspace=aces_space,
                        chromatic_adaptation_transform=None,
                        apply_cctf_encoding=False,
                    ).astype(np.float32)
                if not np.isclose(tint, 1.0):
                    rgb = (rgb * np.asarray([1.0, tint, 1.0], dtype=np.float32)).astype(np.float32)
            prophoto = colour.RGB_to_RGB(
                rgb,
                input_colourspace=aces_space,
                output_colourspace=prophoto_space,
                apply_cctf_decoding=False,
                apply_cctf_encoding=False,
            ).astype(np.float32)
            vector = _find_vector(report, scenario["id"], patch["id"])
            actual = vector["outputs"][ORACLE_CANDIDATE]
            if _f32_bits(tuple(float(value) for value in rgb)) != actual["aces_bits"]:
                raise AssertionError(f"colour CAT02 mismatch for {scenario['id']}/{patch['id']}")
            if _f32_bits(tuple(float(value) for value in prophoto)) != actual["prophoto_bits"]:
                raise AssertionError(f"colour ProPhoto mismatch for {scenario['id']}/{patch['id']}")


def _print_markdown(report: dict[str, Any]) -> None:
    print("| Scenario | Candidate | ACES max abs | ACES RMS | ProPhoto max abs | ProPhoto RMS |")
    print("|---|---|---:|---:|---:|---:|")
    for scenario in report["scenario_summary"]:
        for candidate in CANDIDATES:
            metrics = scenario["metrics"][candidate]
            print(
                f"| {scenario['scenario']} | {candidate} | "
                f"{metrics['aces']['max_abs']:.9g} | {metrics['aces']['rms']:.9g} | "
                f"{metrics['prophoto']['max_abs']:.9g} | {metrics['prophoto']['rms']:.9g} |"
            )


def main(argv: list[str] | None = None) -> int:
    default_fixture = Path(__file__).with_name("fixtures") / "raw_wb_cat_vectors.json"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, default=default_fixture)
    parser.add_argument("--check", action="store_true", help="verify pinned digest and reference bits")
    parser.add_argument("--verify-colour", action="store_true", help="also execute colour-science 0.4.7")
    parser.add_argument("--json", action="store_true", help="print the complete canonical result")
    parser.add_argument("--markdown", action="store_true", help="print the aggregate Markdown table")
    parser.add_argument(
        "--emit-cpp-header",
        type=Path,
        help="write the digest-locked build-only C++ fixture table",
    )
    args = parser.parse_args(argv)

    fixture = json.loads(args.fixture.read_text(encoding="utf-8"))
    if fixture["selected_model"] != ORACLE_CANDIDATE:
        raise AssertionError("fixture must select the pinned oracle's CAT02 transform")
    report = _build_report(fixture)
    digest = _report_digest(report)

    if args.check:
        expected_digest = fixture["expected_report_sha256"]
        if digest != expected_digest:
            raise AssertionError(f"report SHA-256 {digest} != pinned {expected_digest}")
        _verify_reference_bits(fixture, report)
        # These lower bounds prevent the decision fixture from becoming too weak
        # to distinguish the rejected transforms from the selected oracle.
        for rejected in ("android_xyz_scaling", "bradford", "no_adaptation"):
            if report["overall"][rejected]["prophoto"]["max_abs"] < 1.0e-4:
                raise AssertionError(f"fixture no longer distinguishes rejected model {rejected}")
        for diagnostic in (
            "cat02_single_round_tint",
            "cat02_no_reference_skip",
            "cat02_no_tint_skip",
        ):
            if report["overall"][diagnostic]["prophoto"]["max_abs"] <= 0.0:
                raise AssertionError(f"fixture no longer exercises oracle detail {diagnostic}")
    if args.verify_colour:
        _verify_with_colour(fixture, report)
    if args.emit_cpp_header:
        if not args.check:
            raise ValueError("--emit-cpp-header requires --check")
        args.emit_cpp_header.parent.mkdir(parents=True, exist_ok=True)
        args.emit_cpp_header.write_text(
            _cpp_fixture_header(fixture, report), encoding="utf-8", newline="\n"
        )

    if args.json:
        print(json.dumps(report, sort_keys=True, indent=2, allow_nan=False))
    elif args.markdown:
        _print_markdown(report)
    else:
        print(f"raw-wb-cat report_sha256={digest}")
        for candidate, metrics in report["overall"].items():
            print(
                f"{candidate}: prophoto max_abs={metrics['prophoto']['max_abs']:.9g} "
                f"rms={metrics['prophoto']['rms']:.9g}"
            )
        if args.check:
            print("CHECK PASS")
        if args.verify_colour:
            print("COLOUR 0.4.7 ORACLE PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, KeyError, RuntimeError, ValueError) as error:
        print(f"raw-wb-cat: {error}", file=sys.stderr)
        raise SystemExit(1)

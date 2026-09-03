#!/usr/bin/env python3
"""Generate deterministic K75P visual-regression evidence from the pinned oracle.

The numeric authority remains the float32 ``.spkvec`` fixtures. These PNGs make
the same result reviewable by eye and include a deliberately wrong D50 scan plus
a fixed-scale absolute-difference heatmap. Run only from the pinned spektrafilm
c1d0e44 environment documented in ``tools/parity/README.md``.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import tempfile
from pathlib import Path

import colour
import numpy as np
import spkvec
from gen_goldens import (
    ORACLE_COMMIT,
    ORACLE_REPOSITORY,
    OracleProvenance,
    OracleProvenanceError,
    publish_verified_artifacts,
    verify_oracle_provenance,
)
from opt_einsum import contract
from PIL import Image
from PIL import __version__ as pillow_version

HERE = Path(__file__).resolve().parent
OUT = HERE / "goldens" / "viewing_illuminants"
STOCKS = ("kodak_2383", "kodak_2393")
SPACES = (
    ("sRGB", True, "srgb"),
    ("Adobe RGB (1998)", True, "adobe_rgb"),
    ("ProPhoto RGB", True, "prophoto"),
    ("ITU-R BT.2020", True, "rec2020"),
    ("ACES2065-1", True, "aces2065_1"),
    ("sRGB", False, "linear_srgb"),
)
SCALE = 4
GAP = 8
HEAT_GAIN = 8.0


def _scan(
    profile: dict, density_cmy: np.ndarray, viewing: str,
    output_space: str = "sRGB", cctf: bool = True,
) -> np.ndarray:
    from spektrafilm.config import STANDARD_OBSERVER_CMFS
    from spektrafilm.model.emulsion import compute_density_spectral
    from spektrafilm.model.illuminants import standard_illuminant
    from spektrafilm.utils.conversions import density_to_light

    channel_density = np.asarray(profile["data"]["channel_density"], dtype=np.float64)
    base_density = np.asarray(profile["data"]["base_density"], dtype=np.float64)
    illuminant = standard_illuminant(viewing)
    normalization = np.sum(illuminant * STANDARD_OBSERVER_CMFS[:, 1], axis=0)
    density_spectral = compute_density_spectral(
        channel_density, density_cmy, base_density
    )
    light = density_to_light(density_spectral, illuminant)
    xyz = contract("ijk,kl->ijl", light, STANDARD_OBSERVER_CMFS[:]) / normalization
    xyz = 10 ** np.log10(np.fmax(xyz, 0.0) + 1e-10)
    illuminant_xyz = (
        contract("k,kl->l", illuminant, STANDARD_OBSERVER_CMFS[:]) / normalization
    )
    illuminant_xy = colour.XYZ_to_xy(illuminant_xyz)
    rgb = colour.XYZ_to_RGB(
        xyz, colourspace=output_space, apply_cctf_encoding=False,
        illuminant=illuminant_xy,
    )
    if cctf:
        rgb = colour.RGB_to_RGB(
            rgb, output_space, output_space, apply_cctf_decoding=False,
            apply_cctf_encoding=True,
        )
    return np.ascontiguousarray(np.clip(rgb, 0, 1).astype(np.float32))


def _u8(rgb: np.ndarray) -> np.ndarray:
    finite = np.nan_to_num(rgb, nan=0.0, posinf=1.0, neginf=0.0)
    return np.rint(np.clip(finite, 0.0, 1.0) * 255.0).astype(np.uint8)


def _up(rgb: np.ndarray) -> np.ndarray:
    return np.repeat(np.repeat(_u8(rgb), SCALE, axis=0), SCALE, axis=1)


def _heat(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    # Fixed gain, never per-image normalization: visual intensity remains
    # comparable across stocks and regenerations. Black=no change, red/yellow=
    # increasing change, white=clipped at |delta| >= 1/HEAT_GAIN.
    d = np.clip(np.max(np.abs(a.astype(np.float64) - b), axis=2) * HEAT_GAIN,
                0.0, 1.0)
    heat = np.stack(
        (d, np.clip(2.0 * d - 0.5, 0.0, 1.0), np.clip(4.0 * d - 3.0, 0.0, 1.0)),
        axis=2,
    )
    return _up(heat.astype(np.float32))


def _hstack(images: list[np.ndarray]) -> np.ndarray:
    gap = np.full((images[0].shape[0], GAP, 3), 32, dtype=np.uint8)
    parts: list[np.ndarray] = []
    for index, item in enumerate(images):
        if index:
            parts.append(gap)
        parts.append(item)
    return np.concatenate(parts, axis=1)


def _vstack(images: list[np.ndarray]) -> np.ndarray:
    gap = np.full((GAP, images[0].shape[1], 3), 32, dtype=np.uint8)
    parts: list[np.ndarray] = []
    for index, item in enumerate(images):
        if index:
            parts.append(gap)
        parts.append(item)
    return np.concatenate(parts, axis=0)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _load_verified_spektrafilm(
    checkout_path: Path,
) -> tuple[Path, OracleProvenance]:
    """Load spektrafilm only from the exact clean checkout that was verified."""
    provenance = verify_oracle_provenance(checkout_path, ORACLE_COMMIT)
    repository_root = provenance.repository_root
    package_init = repository_root / "src" / "spektrafilm" / "__init__.py"
    if not package_init.is_file():
        raise OracleProvenanceError(
            f"verified checkout has no src/spektrafilm package: {repository_root}"
        )

    sys.path.insert(0, str(repository_root / "src"))
    previous_dont_write_bytecode = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        import spektrafilm
    except Exception as exc:
        raise OracleProvenanceError(
            f"could not import spektrafilm from {repository_root}: "
            f"{type(exc).__name__}: {exc}"
        ) from exc
    finally:
        sys.dont_write_bytecode = previous_dont_write_bytecode

    imported_file = getattr(spektrafilm, "__file__", None)
    if imported_file is None:
        raise OracleProvenanceError(
            "imported spektrafilm has no concrete source file"
        )
    imported_provenance = verify_oracle_provenance(
        Path(imported_file), ORACLE_COMMIT
    )
    if imported_provenance.repository_root != repository_root:
        raise OracleProvenanceError(
            "spektrafilm import resolved from a different checkout: "
            f"verified {repository_root}; imported "
            f"{imported_provenance.repository_root}"
        )
    return repository_root, imported_provenance


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--spektrafilm-src", type=Path, required=True,
        help="pinned spektrafilm checkout root (must be c1d0e44)",
    )
    args = parser.parse_args(argv)
    try:
        source_root, oracle_provenance = _load_verified_spektrafilm(
            args.spektrafilm_src
        )
    except OracleProvenanceError as exc:
        sys.stderr.write(
            "ERROR: refusing unverified spektrafilm oracle checkout.\n"
            f"  {exc}\n"
        )
        return 2

    goldens_root = HERE / "goldens"
    goldens_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=".viewing-illuminants-stage-", dir=goldens_root
    ) as staging_directory:
        staging_dir = Path(staging_directory)
        artifacts: list[tuple[Path, Path]] = []
        rendered: dict[str, dict[str, np.ndarray]] = {}
        metrics: dict[str, dict[str, float]] = {}
        output_refs: dict[str, dict[str, str]] = {}
        for stock in STOCKS:
            case = goldens_root / f"print_{stock}_k75p"
            density = np.ascontiguousarray(
                spkvec.read(case / "print_density_cmy.spkvec"), dtype=np.float32
            )
            profile = json.loads(
                (
                    source_root
                    / "src"
                    / "spektrafilm"
                    / "data"
                    / "profiles"
                    / f"{stock}.json"
                ).read_text(encoding="utf-8")
            )
            if profile["info"]["viewing_illuminant"] != "K75P":
                raise RuntimeError(
                    f"{stock}: pinned profile no longer declares K75P"
                )
            refs = {
                label: _scan(profile, density, "K75P", output_space, cctf)
                for output_space, cctf, label in SPACES
            }
            k75p = refs["srgb"]
            d50 = _scan(profile, density, "D50")
            golden = np.ascontiguousarray(
                spkvec.read(case / "final_rgb.spkvec"), dtype=np.float32
            )
            oracle_error = np.abs(k75p.astype(np.float64) - golden)
            oracle_rms = float(np.sqrt(np.mean(oracle_error * oracle_error)))
            # The stored density is float32 while the full oracle carries its
            # float64 plane into scanning, so this is tolerance-identical.
            if float(oracle_error.max()) > 1e-4 or oracle_rms > 1e-5:
                raise RuntimeError(
                    f"{stock}: regenerated K75P scan exceeds the fixture tolerance"
                )
            delta = np.abs(k75p.astype(np.float64) - d50)
            rendered[stock] = {"k75p": k75p, "d50": d50}
            output_refs[stock] = {}
            for label, ref in refs.items():
                filename = f"scan_ref_{label}.spkvec"
                staged_ref = staging_dir / f"{stock}_{filename}"
                destination = case / filename
                spkvec.write(staged_ref, ref)
                artifacts.append((staged_ref, destination))
                output_refs[stock][label] = hashlib.sha256(
                    staged_ref.read_bytes()
                ).hexdigest()
            metrics[stock] = {
                "k75p_fixture_max_abs": float(oracle_error.max()),
                "k75p_fixture_rms": oracle_rms,
                "d50_vs_k75p_max_abs": float(delta.max()),
                "d50_vs_k75p_rms": float(np.sqrt(np.mean(delta * delta))),
            }

        contact = _hstack([_up(rendered[s]["k75p"]) for s in STOCKS])
        comparison_rows = [
            _hstack(
                [
                    _up(rendered[s]["k75p"]),
                    _up(rendered[s]["d50"]),
                    _heat(rendered[s]["k75p"], rendered[s]["d50"]),
                ]
            )
            for s in STOCKS
        ]
        comparison = _vstack(comparison_rows)
        staged_contact = staging_dir / "k75p_2383_2393.png"
        staged_comparison = staging_dir / "d50_vs_k75p_diff.png"
        contact_destination = OUT / staged_contact.name
        comparison_destination = OUT / staged_comparison.name
        Image.fromarray(contact, mode="RGB").save(
            staged_contact, compress_level=9
        )
        Image.fromarray(comparison, mode="RGB").save(
            staged_comparison, compress_level=9
        )
        artifacts.extend(
            (
                (staged_contact, contact_destination),
                (staged_comparison, comparison_destination),
            )
        )

        manifest = {
            "columns": {
                staged_contact.name: ["Kodak 2383 K75P", "Kodak 2393 K75P"],
                staged_comparison.name: [
                    "K75P",
                    "D50 counterfactual",
                    "abs diff heatmap",
                ],
            },
            "rows": list(STOCKS),
            "heat_gain": HEAT_GAIN,
            "metrics": metrics,
            "output_space_ref_sha256": output_refs,
            "oracle": {
                "repository": ORACLE_REPOSITORY,
                "colour_version": colour.__version__,
                "pillow_version": pillow_version,
            },
            "sha256": {
                staged_contact.name: _sha256(staged_contact),
                staged_comparison.name: _sha256(staged_comparison),
            },
        }
        try:
            # Verify after all oracle calls, before any attested output changes.
            oracle_provenance = verify_oracle_provenance(
                oracle_provenance.repository_root, ORACLE_COMMIT
            )
            manifest["oracle"].update(oracle_provenance.as_manifest())
            staged_manifest = staging_dir / "manifest.json"
            staged_manifest.write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            publish_verified_artifacts(
                artifacts,
                staged_manifest,
                OUT / "manifest.json",
                oracle_provenance,
            )
        except OracleProvenanceError as exc:
            sys.stderr.write(
                "ERROR: oracle provenance changed during visual generation; "
                "new manifest was not published.\n"
                f"  {exc}\n"
            )
            return 2

    print(json.dumps(manifest, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

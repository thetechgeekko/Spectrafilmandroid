# Spektrafilm for Android — golden-vector parity harness.
# Copyright (C) 2026 Spektrafilm Android contributors.
#
# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version. See <https://www.gnu.org/licenses/>.
#
# Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
# spektrafilm.
"""Generate golden intermediate vectors from the Python ``spektrafilm`` engine.

For each parity *case* (a film/print profile pair plus toggles) this script runs
``spektrafilm.simulate`` once per debug *tap*, capturing the intermediate buffer
the engine exposes via ``DebugParams`` (``output_film_log_raw``,
``output_film_density_cmy``, ``output_print_density_cmy``) as well as the final
RGB scan. Each buffer is written to ``goldens/<case>/<tap>.spkvec`` and the case
is described in ``goldens/<case>/manifest.json``.

The C++ engine port reproduces each tap via ``spk_simulate_tap`` and is checked
against these goldens by ``compare_main.cpp`` within a documented tolerance.

This module REQUIRES a working ``spektrafilm`` environment (see the upstream
README — install the package and its data assets). The import is guarded so the
failure mode is a clear, actionable message rather than a traceback. The rest of
the harness (format spec, readers, C++ comparator) does NOT need spektrafilm.

Usage::

    python gen_goldens.py                      # default cases (see CASES below)
    python gen_goldens.py --case print_portra  # one named case
    python gen_goldens.py --list               # list available cases
    python gen_goldens.py --size 64            # test-image edge length (px)
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
GOLDENS_DIR = HERE / "goldens"

# Deterministic seed for the synthetic image and any stochastic engine effects.
SEED = 20260529
ORACLE_REPOSITORY = "https://github.com/andreavolpato/spektrafilm"
ORACLE_COMMIT = "c1d0e44b962d80a51ea096d33faea346e4f3836c"


class OracleProvenanceError(RuntimeError):
    """The selected spektrafilm checkout cannot be trusted as the oracle."""


@dataclass(frozen=True)
class OracleProvenance:
    """Git identity verified for the checkout that supplies spektrafilm."""

    repository_root: Path
    commit: str
    worktree: str = "clean"

    def as_manifest(self) -> dict[str, str]:
        """Return deterministic, path-free manifest provenance.

        ``worktree`` follows normal Git cleanliness: repository-ignored files
        are intentionally excluded, while every non-ignored untracked file is
        rejected.
        """
        return {"commit": self.commit, "worktree": self.worktree}


def _run_git(location: Path, *args: str, safe_directory: Path) -> str:
    """Run a read-only Git query from *location*, failing closed on any error."""
    candidate = Path(location).resolve()
    working_directory = candidate if candidate.is_dir() else candidate.parent
    if not working_directory.is_dir():
        raise OracleProvenanceError(
            f"oracle path does not exist or has no parent directory: {candidate}"
        )
    trusted_root = Path(safe_directory).resolve()
    if not trusted_root.is_dir():
        raise OracleProvenanceError(
            f"oracle safe.directory is not a directory: {trusted_root}"
        )
    try:
        working_directory.relative_to(trusted_root)
    except ValueError as exc:
        raise OracleProvenanceError(
            f"Git query path {working_directory} is outside exact safe.directory "
            f"{trusted_root}"
        ) from exc

    try:
        # Ambient Git variables can redirect repository discovery or inject
        # configuration. Preserve the ordinary process environment but remove
        # every GIT_* override before applying the explicit read-only policy.
        environment = {
            name: value
            for name, value in os.environ.items()
            if not name.upper().startswith("GIT_")
        }
        environment["GIT_CONFIG_GLOBAL"] = os.devnull
        environment["GIT_CONFIG_NOSYSTEM"] = "1"
        environment["GIT_OPTIONAL_LOCKS"] = "0"
        result = subprocess.run(
            ["git", "-c", f"safe.directory={trusted_root.as_posix()}", *args],
            cwd=working_directory,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=environment,
            check=False,
        )
    except OSError as exc:
        raise OracleProvenanceError(
            f"could not execute git {' '.join(args)} at {working_directory}: {exc}"
        ) from exc

    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip() or "no Git diagnostic"
        raise OracleProvenanceError(
            f"git {' '.join(args)} failed at {working_directory} "
            f"(exit {result.returncode}): {detail}"
        )
    return result.stdout


def _find_git_marker_root(location: Path) -> Path:
    """Find the nearest checkout root using a .git directory or gitfile."""
    candidate = Path(location).resolve()
    if not candidate.exists():
        raise OracleProvenanceError(f"oracle path does not exist: {candidate}")
    current = candidate if candidate.is_dir() else candidate.parent
    while True:
        marker = current / ".git"
        if marker.is_dir() or marker.is_file():
            return current
        if marker.exists() or marker.is_symlink():
            raise OracleProvenanceError(
                f"unsupported .git marker at oracle checkout root: {marker}"
            )
        parent = current.parent
        if parent == current:
            break
        current = parent
    raise OracleProvenanceError(
        f"oracle path is not inside a Git checkout with a .git marker: {candidate}"
    )


def _resolve_git_repository_root(location: Path) -> Path:
    """Resolve the enclosing worktree root for a checkout path."""
    candidate = Path(location).resolve()
    marker_root = _find_git_marker_root(candidate)
    root_text = _run_git(
        candidate,
        "rev-parse",
        "--show-toplevel",
        safe_directory=marker_root,
    ).strip()
    if not root_text:
        raise OracleProvenanceError(
            f"git rev-parse --show-toplevel returned no path for {candidate}"
        )
    repository_root = Path(root_text).resolve()
    if repository_root != marker_root:
        raise OracleProvenanceError(
            f"Git resolved top-level {repository_root}, but nearest .git marker "
            f"belongs to {marker_root}"
        )
    try:
        candidate.relative_to(repository_root)
    except ValueError as exc:
        raise OracleProvenanceError(
            f"oracle path {candidate} is outside resolved Git root {repository_root}"
        ) from exc
    return repository_root


def verify_oracle_provenance(
    location: Path, expected_commit: str = ORACLE_COMMIT
) -> OracleProvenance:
    """Verify exact HEAD and a clean tracked/untracked oracle worktree."""
    repository_root = _resolve_git_repository_root(location)
    actual_commit = _run_git(
        repository_root,
        "rev-parse",
        "HEAD",
        safe_directory=repository_root,
    ).strip()
    if actual_commit != expected_commit:
        raise OracleProvenanceError(
            f"oracle checkout HEAD mismatch at {repository_root}: "
            f"expected {expected_commit}; found {actual_commit or '<empty>'}"
        )

    # Repository-owned .gitignore/info-exclude rules retain normal Git meaning;
    # global/system excludes are disabled by _run_git and cannot hide files.
    status = _run_git(
        repository_root,
        "-c",
        "core.excludesFile=",
        "-c",
        "core.fsmonitor=false",
        "-c",
        "core.untrackedCache=false",
        "status",
        "--porcelain=v1",
        "--untracked-files=all",
        "--ignore-submodules=none",
        safe_directory=repository_root,
    ).rstrip("\r\n")
    if status:
        changes = "; ".join(status.splitlines())
        raise OracleProvenanceError(
            f"oracle checkout is dirty at {repository_root}; tracked, staged, "
            f"and untracked changes are forbidden: {changes}"
        )

    return OracleProvenance(
        repository_root=repository_root,
        commit=actual_commit,
    )


def publish_verified_artifacts(
    artifacts: list[tuple[Path, Path]],
    staged_manifest: Path,
    manifest_destination: Path,
    oracle_provenance: OracleProvenance,
) -> OracleProvenance:
    """Publish staged outputs with the attesting manifest atomically last.

    Individual artifact replacements are atomic on the shared filesystem, but
    the set cannot be replaced in one operation. The old manifest is therefore
    removed before the first artifact changes, and the new manifest is exposed
    only after every artifact is in place and provenance is verified again.
    """
    manifest_destination.parent.mkdir(parents=True, exist_ok=True)
    manifest_destination.unlink(missing_ok=True)
    try:
        for staged_path, destination in artifacts:
            destination.parent.mkdir(parents=True, exist_ok=True)
            staged_path.replace(destination)

        final_provenance = verify_oracle_provenance(
            oracle_provenance.repository_root, oracle_provenance.commit
        )
        staged_manifest.replace(manifest_destination)
    except BaseException:
        # A partial artifact set must never retain an older or staged attestation.
        manifest_destination.unlink(missing_ok=True)
        raise
    return final_provenance

# Taps exposed by spektrafilm DebugParams, in pipeline order. The final RGB scan
# is captured with debug off ("final_rgb"). The mapping value is the DebugParams
# field that selects the tap; None means "run the full pipeline, no tap".
TAPS = {
    "film_log_raw": "output_film_log_raw",
    "film_density_cmy": "output_film_density_cmy",
    "print_density_cmy": "output_print_density_cmy",
    "final_rgb": None,
}

# Taps that do not exist when scan_film is on (no print stage). For scan_film
# cases we capture the negative taps and the final RGB scan only.
SCAN_FILM_SKIP_TAPS = {"print_density_cmy"}


@dataclass
class Case:
    """A single parity case: one profile pair + a set of deterministic toggles."""

    case_id: str
    film_profile: str
    print_profile: str
    scan_film: bool = False
    # Engine toggles. We deliberately disable stochastic + spatial effects by
    # default so goldens are bit-stable across hosts; grain-on cases are opt-in
    # and compared by statistics/tolerance (see README "Grain & stochastic taps").
    deactivate_stochastic_effects: bool = True
    deactivate_spatial_effects: bool = True
    grain_active: bool = False
    # Crop / resize geometry (io params). Defaults are a strict no-op; non-default
    # values exercise the crop_and_rescale (crop + cubic upscale) preprocess.
    crop: bool = False
    crop_center: tuple = (0.5, 0.5)
    crop_size: tuple = (0.1, 0.1)
    upscale_factor: float = 1.0
    # Auto-exposure (camera.auto_exposure / auto_exposure_method). Default OFF for
    # determinism; a non-default case turns it ON to exercise the metering stage.
    auto_exposure: bool = False
    auto_exposure_method: str = "center_weighted"
    # Camera lens blur in micrometres (camera.lens_blur_um). Default 0.0 (strict
    # no-op). A non-default case sets it to exercise the µm Gaussian lens-blur pass
    # in FilmingStage.expose (applied between the diffusion filter and halation).
    # lens_blur_um is only honoured when deactivate_spatial_effects is False (the
    # oracle's digest_params zeroes it otherwise), so a lens-blur case must keep
    # spatial effects on.
    lens_blur_um: float = 0.0
    # Camera optical diffusion filter (camera.diffusion_filter). Default OFF
    # (active=False is a strict no-op). A non-default case turns it ON with a
    # real filter family + strength to exercise apply_diffusion_filter_um END TO
    # END through the pipeline (filming.expose), not just in stage isolation.
    # The oracle's digest_params zeroes camera.diffusion_filter.active when
    # deactivate_spatial_effects=True, so a diffusion full-pipeline case MUST keep
    # spatial effects on (deactivate_spatial_effects=False), exactly like the
    # lens-blur case.
    diffusion_active: bool = False
    diffusion_family: str = "black_pro_mist"
    diffusion_strength: float = 0.5
    diffusion_spatial_scale: float = 1.0
    diffusion_halo_warmth: float = 0.0
    # Spectral-domain Gaussian blur of the reconstructed-spectra LUT
    # (settings.spectral_gaussian_blur). Default 0.0 is a strict no-op. A non-zero
    # value blurs HANATOS2025_SPECTRA_LUT along its spectral axis with
    # scipy.ndimage.gaussian_filter(sigma=(0,0,sigma)) inside
    # compute_hanatos2025_tc_lut, changing the filming tc_lut and therefore every
    # downstream tap. It is NOT gated by deactivate_spatial_effects (it lives in
    # the LUT-build path, not the per-pixel spatial branch), so a blur case can
    # keep spatial/grain off and stay bit-stable.
    spectral_gaussian_blur: float = 0.0
    # Hanatos2025 sensitivity-adaptation toggles
    # (settings.apply_hanatos2025_adaptation_window / _surface). The defaults match
    # the schema (window ON, surface OFF) so a default case reproduces the existing
    # goldens. A non-default case turns the surface ON: the filming tc_lut is
    # multiplied by 2**surface, where surface is the per-LUT-cell, per-channel poly4
    # log-exposure correction (eval_poly4_log_exposure_surface) evaluated at the
    # film reference-illuminant chromaticity, inside compute_hanatos2025_tc_lut. It
    # lives in the LUT-build path (not the spatial branch), so a surface case can
    # keep spatial/grain off and stay bit-stable.
    apply_hanatos_window: bool = True
    apply_hanatos_surface: bool = False
    # Camera UV/IR cut band-pass filter (camera.filter_uv / filter_ir), each
    # (amp, wavelength_nm, width_nm). Defaults (amp 0) are a strict no-op. A non-
    # default case sets a non-zero amplitude to exercise compute_band_pass_filter in
    # FilmingStage._rgb_to_film_raw: the band-pass multiplies the profile sensitivity
    # (with per-channel white-balance normalisation) BEFORE the spectra contraction,
    # changing the filming tc_lut and every downstream tap. It lives in the LUT-build
    # path (not the spatial branch), so a band-pass case can keep spatial/grain off
    # and stay bit-stable.
    filter_uv: tuple = (0.0, 410.0, 8.0)
    filter_ir: tuple = (0.0, 675.0, 15.0)
    # Enlarger PREFLASH (enlarger.preflash_exposure / preflash_y_filter_shift /
    # preflash_m_filter_shift): a uniform pre-exposure flash of the print paper
    # before the image exposure. Default preflash_exposure 0.0 is a strict no-op
    # (the oracle's `if preflash_exposure > 0` guard returns a zero raw_preflash).
    # A non-default case turns it on with its own y/m filter shifts to exercise
    # printing.py::_compute_raw_preflash + preflash_filtered_illuminant. Lives in
    # the PRINT route only (scan_film=False), so a preflash case must set
    # scan_film=False; spatial/grain stay off so it is deterministic/bit-stable.
    preflash_exposure: float = 0.0
    preflash_y_filter_shift: float = 0.0
    preflash_m_filter_shift: float = 0.0
    # Camera exposure compensation (camera.exposure_compensation_ev) + the enlarger
    # midgray-balance toggles (enlarger.print_exposure_compensation /
    # normalize_print_exposure, both schema-default True). On the PRINT route these
    # feed PrintingStage._compute_exposure_factor_midgray (printing.py c1d0e44
    # L104-118): the midgray exposure factor is selected by a 4-case branch over
    # (print_exposure_compensation, normalize_print_exposure) using the UNcompensated
    # midgray (rgb=0.184) and the COMPENSATED midgray (rgb=0.184*2^ev) from
    # FilmingStage._compute_density_spectral_midgray_to_balance_print (filming.py
    # c1d0e44 L125-134). At exposure_compensation_ev=0 the compensated gray equals
    # the uncompensated gray -> the factor is byte-identical to the EV=0 default, so
    # all existing print goldens are unchanged. A non-zero EV with compensation ON
    # exercises the bug-fix path. exposure_compensation_ev ALSO scales the filming
    # raw (raw *= 2^ev, filming.py L57), so it changes the negative taps too.
    exposure_compensation_ev: float = 0.0
    print_exposure_compensation: bool = True
    normalize_print_exposure: bool = True
    # Scanner BLACK/WHITE corrections (scanner.black_correction / white_correction,
    # black_level / white_level): a scan-time tone anchor that maps the measured CIE
    # Y at the reference black/white densities to the (sRGB-decoded) target levels.
    # Default OFF (both corrections false) is a STRICT no-op. The oracle
    # (runtime/services/color_reference.py::ColorReferenceService) spans three stages
    # but route-gates each: filming exposure correction (scan_film + POSITIVE film),
    # printing exposure correction (print route + NEGATIVE paper), and the scanning
    # XYZ correction (every route EXCEPT scan_film + negative film). A correction
    # case therefore picks a route that fires the corrections: the print route
    # (negative paper) exercises the printing-exposure + xyz corrections; a
    # scan_film case with a POSITIVE film exercises the filming-exposure + xyz
    # corrections. The references / m,q are profile + route dependent but constant
    # across pixels, so a correction case stays deterministic/bit-stable.
    scanner_black_correction: bool = False
    scanner_white_correction: bool = False
    scanner_black_level: float = 0.01
    scanner_white_level: float = 0.98
    # DIR couplers (film_render.dir_couplers.active). Default True (schema default).
    # A case may turn them OFF to isolate a stage from the coupler path. This is the
    # one knob the scan_provia_bwcorr case sets: the POSITIVE-film DIR-coupler branch
    # is a separate, currently un-parity-gated code path in the native engine (all
    # other goldens are negative film), so the positive-film correction case keeps
    # the couplers OFF to isolate the SCANNER correction it is testing.
    dir_couplers_active: bool = True
    # Highlight boost (film_render.halation.boost_ev / boost_range / protect_ev): the
    # pre-clip highlight reconstruction (utils/numba_boost_hightlights.boost_highlights)
    # that FilmingStage.expose applies to the float64 raw irradiance right after the
    # exposure-compensation scale and BEFORE the diffusion filter / lens blur / halation
    # (filming.py:58-60), with the default midgray = 0.184. Default boost_ev 0.0 is a
    # strict no-op (boost_highlights returns x unchanged). It is NOT gated by
    # deactivate_spatial_effects (params_builder.py only zeroes the scatter/halation
    # sigmas, never boost_ev), so a boost case can keep spatial/grain OFF and stay
    # deterministic/bit-stable. A non-default case lifts every raw value above
    # raw_x0 = clip(0.184 * 2^protect_ev, 0, max(raw)) by the exponential boost curve,
    # changing the film taps and final_rgb.
    boost_ev: float = 0.0
    boost_range: float = 0.3
    protect_ev: float = 4.0
    # Halation-only kill switch. When True, ONLY the three halation-object spatial
    # sigmas that the oracle's deactivate_spatial_effects path zeroes
    # (params_builder.py c1d0e44 L73-75: film_render.halation.scatter_core_um /
    # scatter_tail_um / halation_first_sigma_um) are set to (0,0,0), WHILE every other
    # spatial effect (camera.lens_blur_um, DIR-coupler diffusion_size_um) is left
    # active. It decouples the halation/scatter kernels from the rest of the spatial
    # branch, so a case can keep deactivate_spatial_effects=False (lens blur alive) yet
    # produce NO halation. The zeroing MUST happen AFTER digest_params:
    # _apply_halation_preset re-seeds halation_first_sigma_um from the profile preset
    # during digest (params_builder.py L198), so gen_goldens digests, zeroes these
    # three, then simulates with digest_params_first=False (see _run_tap). Grain/glare
    # stay off so the case is still deterministic/bit-stable.
    halation_off: bool = False
    # Optional scanner-LUT route fixtures. These are generated in addition to
    # the direct final_rgb tap so tests can compare like-for-like against the
    # pinned oracle's intentionally approximate PCHIP path, rather than hiding
    # its interpolation error inside a widened LUT-vs-direct tolerance.
    scanner_lut_resolutions: tuple = ()
    notes: str = ""
    taps: tuple = field(default=tuple(TAPS.keys()))


# Initial parity matrix (mirrors cases.md). Kept small + deterministic.
CASES = [
    Case(
        case_id="print_portra",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=False,
        notes="Baseline negative->print->scan, all stochastic/spatial effects off.",
    ),
    Case(
        case_id="print_kodak_2383_k75p",
        film_profile="kodak_portra_400",
        print_profile="kodak_2383",
        scan_film=False,
        scanner_lut_resolutions=(17,),
        notes="Kodak 2383 print scanned under its declared Kinoton 75P viewing "
              "illuminant. Locks the profile-driven K75P spectral integral and "
              "K75P-to-output-white CAT without stochastic/spatial effects.",
    ),
    Case(
        case_id="print_kodak_2393_k75p",
        film_profile="kodak_portra_400",
        print_profile="kodak_2393",
        scan_film=False,
        scanner_lut_resolutions=(17,),
        notes="Kodak 2393 print scanned under its declared Kinoton 75P viewing "
              "illuminant. Paired with the 2383 case so both affected bundled "
              "profiles are independently oracle-gated.",
    ),
    Case(
        case_id="scan_portra",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=True,
        notes="scan_film route: negative scanned directly, no print stage.",
    ),
    Case(
        case_id="print_ektar",
        film_profile="kodak_ektar_100",
        print_profile="kodak_supra_endura",
        scan_film=False,
        notes="Second negative/paper pair to catch profile-coupled regressions.",
    ),
    Case(
        case_id="scan_portra_spatial",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=True,
        deactivate_spatial_effects=False,   # halation + in-emulsion scatter + coupler diffusion ON
        deactivate_stochastic_effects=True,  # grain OFF -> still deterministic/bit-stable
        grain_active=False,
        notes="scan_film with spatial effects ON (halation/scatter/diffusion), grain off. "
              "Deterministic target for porting the spatial branches.",
    ),
    Case(
        case_id="scan_portra_lensblur",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=True,
        deactivate_spatial_effects=False,   # required: keeps camera.lens_blur_um alive
        deactivate_stochastic_effects=True,  # grain OFF -> deterministic/bit-stable
        grain_active=False,
        lens_blur_um=1650.0,                # NON-default camera lens blur (µm)
        notes="scan_film with the camera lens blur ON (camera.lens_blur_um=1650µm) "
              "plus the rest of the spatial branch (halation/scatter/coupler "
              "diffusion). Exercises apply_gaussian_blur_um in FilmingStage.expose, "
              "applied between the optical diffusion filter and halation: a µm "
              "Gaussian with sigma = lens_blur_um/pixel_size_um broadcast across the "
              "3 channels. For the 64px image pixel_size_um=546.875, so sigma~=3.017 "
              "px -> the Young-van Vliet IIR path (sigma>=SMALL_SIGMA_MAX=3) is "
              "exercised, a clearly non-trivial blur. Grain off so it stays "
              "deterministic/bit-stable.",
    ),
    Case(
        case_id="scan_portra_crop",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=True,
        deactivate_spatial_effects=True,
        deactivate_stochastic_effects=True,
        grain_active=False,
        crop=True,
        crop_center=(0.45, 0.55),
        crop_size=(0.6, 0.5),
        upscale_factor=1.75,
        notes="scan_film with a NON-default crop + cubic upscale geometry stage "
              "(crop_and_rescale): crop_center=(0.45,0.55), crop_size=(0.6,0.5), "
              "upscale_factor=1.75. Spatial/grain off so it stays bit-stable; "
              "exercises the crop integer-slice + skimage rescale(order=3) port.",
    ),
    Case(
        case_id="scan_portra_downscale",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=True,
        deactivate_spatial_effects=True,
        deactivate_stochastic_effects=True,
        grain_active=False,
        upscale_factor=0.5,
        notes="scan_film with a sub-unity upscale_factor=0.5 (MINIFICATION). "
              "Exercises the skimage.transform.rescale anti-aliasing PREFILTER: for "
              "an output dimension that shrinks, skimage defaults anti_aliasing=True "
              "and runs scipy.ndimage.gaussian_filter (sigma = max(0,(in/out-1)/2) = "
              "0.5 per spatial axis, mode='mirror') BEFORE the order=3 cubic zoom. "
              "The 64x64 input is downsized to 32x32. Spatial/grain off so it stays "
              "bit-stable; gates the downscale-prefilter parity path.",
    ),
    Case(
        case_id="scan_portra_autoexp",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=True,
        deactivate_spatial_effects=True,
        deactivate_stochastic_effects=True,
        grain_active=False,
        auto_exposure=True,                 # NON-default: metering ON
        auto_exposure_method="center_weighted",  # schema default pattern
        notes="scan_film with AUTO-EXPOSURE ON (center_weighted metering). "
              "Exercises FilmingStage.auto_exposure in pipeline._preprocess: the "
              "image is metered (small_preview luminance -> -log2(Y_meter/0.184) "
              "EV) and globally scaled by 2**ev before crop/rescale. For the 64px "
              "synthetic image small_preview is a no-op (long edge 64 <= 256), so "
              "metering runs on the full image. Spatial/grain off so it stays "
              "bit-stable.",
    ),
    Case(
        case_id="scan_portra_autoexp_matrix",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=True,
        deactivate_spatial_effects=True,
        deactivate_stochastic_effects=True,
        grain_active=False,
        auto_exposure=True,                 # NON-default: metering ON
        auto_exposure_method="matrix",      # NON-center_weighted pattern
        notes="scan_film with AUTO-EXPOSURE ON using the MATRIX metering pattern "
              "(a NON-center_weighted pattern; scan_portra_autoexp only covers "
              "center_weighted). Exercises the 5x5 raised-cosine zone-weighted "
              "branch of utils.autoexposure.measure_autoexposure_ev: the image is "
              "split into a 5x5 grid, each zone mean weighted by 0.5*(1+cos(pi*"
              "dist)) of its normalised distance from centre, the weighted average "
              "/ 0.184 -> -log2(exposure) EV, then the image is globally scaled by "
              "2**ev before crop/rescale. For the 64px image small_preview is a "
              "no-op (long edge 64 <= 256). Spatial/grain off so it stays "
              "bit-stable. Tested by tests/test_autoexposure.cpp.",
    ),
    Case(
        case_id="scan_diffusion",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=True,
        deactivate_spatial_effects=False,   # required: keeps camera.diffusion_filter alive
        deactivate_stochastic_effects=True,  # grain OFF -> deterministic/bit-stable
        grain_active=False,
        diffusion_active=True,              # NON-default: camera optical diffusion ON
        diffusion_family="black_pro_mist",
        diffusion_strength=0.8,             # NON-default strength (schema default is 0.5)
        diffusion_spatial_scale=1.0,
        diffusion_halo_warmth=0.0,
        notes="scan_film with the camera OPTICAL DIFFUSION FILTER ON end-to-end "
              "(camera.diffusion_filter: active=True, family=black_pro_mist, "
              "strength=0.8 -- a NON-default strength) plus the rest of the spatial "
              "branch (halation/scatter/coupler diffusion), grain off. Exercises "
              "apply_diffusion_filter_um inside FilmingStage.expose run through the "
              "WHOLE pipeline (the diffusion_bpm case only tests the filter in "
              "isolation). The oracle's digest_params zeroes "
              "camera.diffusion_filter.active under deactivate_spatial_effects=True, "
              "so spatial effects are kept on. Grain off so it stays "
              "deterministic/bit-stable. Tested by tests/test_diffusion_e2e.cpp.",
    ),
    Case(
        case_id="scan_spectral_blur",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=True,
        deactivate_spatial_effects=True,    # blur lives in the LUT build, not the
        deactivate_stochastic_effects=True, # spatial branch -> can stay all-off
        grain_active=False,
        spectral_gaussian_blur=5.0,         # NON-default: sigma=5 (band-index units)
        notes="scan_film with the SPECTRAL GAUSSIAN BLUR ON "
              "(settings.spectral_gaussian_blur=5.0). Exercises the optional blur in "
              "compute_hanatos2025_tc_lut: HANATOS2025_SPECTRA_LUT is blurred along "
              "its spectral axis (axis 2) with scipy.ndimage.gaussian_filter("
              "sigma=(0,0,5.0)) (SciPy defaults order=0, mode='reflect', "
              "truncate=4.0) BEFORE the sensitivity contraction, changing the "
              "filming tc_lut and therefore film_log_raw / film_density_cmy / "
              "final_rgb. The blur is independent of deactivate_spatial_effects "
              "(it is in the LUT-build path, not the per-pixel spatial branch), so "
              "spatial + grain are kept off and the case stays deterministic/"
              "bit-stable. Tested by tests/test_spectral_blur_e2e.cpp.",
    ),
    Case(
        case_id="scan_portra_surface",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=True,
        deactivate_spatial_effects=True,    # surface lives in the LUT build, not the
        deactivate_stochastic_effects=True, # spatial branch -> can stay all-off
        grain_active=False,
        apply_hanatos_window=True,          # window stays ON (schema default)
        apply_hanatos_surface=True,         # NON-default: log-exposure surface ON
        notes="scan_film with the HANATOS2025 LOG-EXPOSURE-CORRECTION SURFACE ON "
              "(settings.apply_hanatos2025_adaptation_surface=True; window left ON). "
              "Exercises the apply_surface branch of compute_hanatos2025_tc_lut: "
              "after the window-weighted spectra contraction, the filming tc_lut is "
              "multiplied per-cell, per-channel by 2**surface, where surface = "
              "eval_poly4_log_exposure_surface(profile.hanatos2025_adaptation_surface"
              "_params, illuminant_xy=_illuminant_to_xy(film.reference_illuminant), "
              "model='poly4') — a degree-4 2D polynomial over the (L,L) tc grid, "
              "centred at tri2quad(illuminant_xy), passed through hanika_sigmoid("
              "raw, max=2.0). This changes the filming tc_lut and therefore "
              "film_log_raw / film_density_cmy / final_rgb. The surface is "
              "independent of deactivate_spatial_effects (it is in the LUT-build "
              "path), so spatial + grain are kept off and the case stays "
              "deterministic/bit-stable. Tested by tests/test_hanatos_surface_e2e.cpp.",
    ),
    Case(
        case_id="scan_portra_nowindow",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=True,
        deactivate_spatial_effects=True,
        deactivate_stochastic_effects=True,
        grain_active=False,
        apply_hanatos_window=False,         # NON-default: erf4 window OFF
        apply_hanatos_surface=False,
        notes="scan_film with the HANATOS2025 ERF4 WINDOW OFF "
              "(settings.apply_hanatos2025_adaptation_window=False, surface also off). "
              "Exercises the apply_window=False branch of compute_hanatos2025_tc_lut: "
              "the spectra LUT is contracted against the BARE sensitivity (no erf4 "
              "band-pass and no white-balance normalisation), changing the filming "
              "tc_lut and therefore film_log_raw / film_density_cmy / final_rgb. "
              "Confirms the window toggle is genuinely wired (its default-on path is "
              "the existing scan_portra golden). Independent of the spatial branch, "
              "so spatial + grain stay off and the case is deterministic/bit-stable. "
              "Tested by tests/test_hanatos_surface_e2e.cpp.",
    ),
    Case(
        case_id="scan_portra_uvir",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=True,
        deactivate_spatial_effects=True,
        deactivate_stochastic_effects=True,
        grain_active=False,
        filter_uv=(1.0, 410.0, 8.0),        # NON-default: UV cut ON (amp=1)
        filter_ir=(1.0, 675.0, 15.0),       # NON-default: IR cut ON (amp=1)
        notes="scan_film with the CAMERA UV/IR CUT BAND-PASS ON "
              "(camera.filter_uv=(1,410,8), filter_ir=(1,675,15)). Exercises "
              "compute_band_pass_filter in FilmingStage._rgb_to_film_raw: the "
              "band-pass = filter_uv*filter_ir (each 1-amp + amp*sigmoid_erf) "
              "multiplies the profile sensitivity with a per-channel white-balance "
              "normalisation against the film reference illuminant, BEFORE the "
              "spectra contraction inside compute_hanatos2025_tc_lut. It changes the "
              "filming tc_lut and therefore film_log_raw / film_density_cmy / "
              "final_rgb. Lives in the LUT-build path (not the spatial branch), so "
              "spatial + grain stay off and the case is deterministic/bit-stable. "
              "Tested by tests/test_camera_uvir_e2e.cpp.",
    ),
    Case(
        case_id="print_portra_preflash",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=False,                    # PRINT route: preflash is a print-stage effect
        deactivate_spatial_effects=True,
        deactivate_stochastic_effects=True,
        grain_active=False,
        preflash_exposure=0.15,             # NON-default: enlarger preflash ON
        preflash_y_filter_shift=-10.0,      # NON-default preflash Y filter shift (Kodak CC)
        preflash_m_filter_shift=5.0,        # NON-default preflash M filter shift (Kodak CC)
        notes="print route with the ENLARGER PREFLASH ON "
              "(enlarger.preflash_exposure=0.15, preflash_y_filter_shift=-10, "
              "preflash_m_filter_shift=5). Exercises printing.py::_compute_raw_preflash "
              "+ filter_enlarger_source.preflash_filtered_illuminant: a uniform "
              "pre-exposure flash of the print paper. The preflash filtered illuminant "
              "is color_enlarger(enlarger source, CC=[c_neutral, m_neutral+preflash_m, "
              "y_neutral+preflash_y]) — its OWN filter shifts, NOT the image-exposure "
              "shifts. print_expose adds a constant per-channel raw_preflash 3-vector "
              "(= sum_l 10^-base_density[l] * preflash_illuminant[l] * sens[l,k]) * "
              "preflash_exposure to the print raw, AFTER the midgray factor and BEFORE "
              "the log10, changing print_density_cmy and final_rgb. Print stage only "
              "(scan_film=False); spatial+grain off so it is deterministic/bit-stable. "
              "Tested by tests/test_preflash_e2e.cpp.",
    ),
    Case(
        case_id="print_portra_bwcorr",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=False,                    # PRINT route: negative paper (endura).
        deactivate_spatial_effects=True,
        deactivate_stochastic_effects=True,
        grain_active=False,
        scanner_black_correction=True,      # NON-default: both corrections ON
        scanner_white_correction=True,
        scanner_black_level=0.01,
        scanner_white_level=0.98,
        notes="print route with the SCANNER BLACK/WHITE CORRECTIONS ON "
              "(scanner.black_correction=white_correction=True, black_level=0.01, "
              "white_level=0.98). Exercises runtime/services/color_reference.py on "
              "the print route (negative paper): the PRINTING exposure correction "
              "(black_white_printing_exposure_correction multiplies the print raw by "
              "10**(le(density_midgray_corrected) - le(density_midgray)) re-anchoring "
              "midgray on the print characteristic) AND the SCANNING XYZ correction "
              "(black_white_xyz_correction rescales each pixel's XYZ by "
              "clip(m*Y+q,0,1)/(Y+1e-10)). The references y_black/y_white come from "
              "develop_simple of the print reference log_raw (= _film_cmy_to_print_"
              "log_raw of the film black/white reference CMY through the live "
              "enlarger) -> cmy_to_log_xyz on the print profile; the levels are "
              "sRGB-decoded (_remove_sRGB_cctf). Both corrections are constant across "
              "pixels (profile + route dependent only), so spatial+grain stay off and "
              "the case is deterministic/bit-stable. Changes print_density_cmy "
              "(printing exposure correction) + final_rgb (both). Tested by "
              "tests/test_scanner_bwcorr_e2e.cpp.",
    ),
    Case(
        case_id="scan_provia_bwcorr",
        film_profile="fujifilm_provia_100f",  # POSITIVE film -> filming corr fires
        print_profile="kodak_portra_endura",
        scan_film=True,                     # scan_film route on a POSITIVE film.
        deactivate_spatial_effects=True,
        deactivate_stochastic_effects=True,
        grain_active=False,
        scanner_black_correction=True,      # NON-default: both corrections ON
        scanner_white_correction=True,
        scanner_black_level=0.01,
        scanner_white_level=0.98,
        dir_couplers_active=False,          # isolate the SCANNER correction (the
                                            # positive-film DIR-coupler branch is a
                                            # separate, un-gated native code path).
        notes="scan_film route on a POSITIVE film (fujifilm_provia_100f) with the "
              "SCANNER BLACK/WHITE CORRECTIONS ON. Exercises the OTHER two "
              "color_reference.py branches: the FILMING exposure correction "
              "(black_white_filming_exposure_correction, active only for scan_film + "
              "positive film: raw *= 1/exposure_correction re-anchoring midgray on "
              "the film characteristic, applied in filming.expose after halation, "
              "before log10) AND the SCANNING XYZ correction. Here the references "
              "y_black/y_white come from the FILM density curves directly "
              "(cmy_black = nanmax(film.density_curves), cmy_white = 0) -> "
              "cmy_to_log_xyz on the film profile. Together with print_portra_bwcorr "
              "this covers all THREE correction code paths. DIR couplers are OFF: the "
              "positive-film DIR-coupler branch is a separate, currently un-parity-"
              "gated native code path (every other golden is negative film), so the "
              "couplers are kept off to ISOLATE the scanner correction under test "
              "(with couplers on, the positive-film develop itself diverges ~0.32 "
              "independently of this correction). Positive film + spatial/grain off "
              "keep it deterministic/bit-stable. Changes film_log_raw / "
              "film_density_cmy (filming correction) + final_rgb (both). Tested by "
              "tests/test_scanner_bwcorr_e2e.cpp.",
    ),
    Case(
        case_id="print_portra_evcomp",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=False,                    # PRINT route: the midgray balance fires.
        deactivate_spatial_effects=True,
        deactivate_stochastic_effects=True,
        grain_active=False,
        exposure_compensation_ev=1.5,       # NON-default camera EV (the bug trigger).
        print_exposure_compensation=True,   # schema default ON
        normalize_print_exposure=True,      # schema default ON -> branch returns
                                            # factor_midgray_comp (compensated only).
        notes="print route with a NON-ZERO camera exposure compensation "
              "(camera.exposure_compensation_ev=1.5) and BOTH enlarger midgray "
              "toggles at their schema defaults (print_exposure_compensation=True, "
              "normalize_print_exposure=True). Exercises the print EV-compensation "
              "bug fix: PrintingStage._compute_exposure_factor_midgray (printing.py "
              "c1d0e44 L104-118) selects factor_midgray_comp (the comp+normalize "
              "branch), where factor_midgray_comp = _exposure_factor(...) of the "
              "COMPENSATED midgray density (rgb=0.184*2^ev) from "
              "FilmingStage._compute_density_spectral_midgray_to_balance_print "
              "(filming.py c1d0e44 L125-134). Previously the native engine ignored "
              "exposure_compensation_ev here and always returned the UNcompensated "
              "factor, mis-balancing the print whenever EV!=0. The EV also scales the "
              "filming raw (raw*=2^ev), so film_log_raw/film_density_cmy change too. "
              "Spatial+grain off so it is deterministic/bit-stable. Tested by "
              "tests/test_print_evcomp_e2e.cpp.",
    ),
    Case(
        case_id="print_portra_evcomp_nonorm",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=False,                    # PRINT route.
        deactivate_spatial_effects=True,
        deactivate_stochastic_effects=True,
        grain_active=False,
        exposure_compensation_ev=1.5,       # same NON-default EV as the sibling case
        print_exposure_compensation=True,   # comp ON
        normalize_print_exposure=False,     # normalize OFF -> the OTHER branch:
                                            # factor = factor_midgray_comp / factor_midgray.
        notes="print route with camera.exposure_compensation_ev=1.5, "
              "print_exposure_compensation=True, normalize_print_exposure=FALSE. "
              "Locks the SECOND midgray branch of "
              "PrintingStage._compute_exposure_factor_midgray (printing.py c1d0e44 "
              "L111-112): comp && !normalize returns factor_midgray_comp / "
              "factor_midgray (the ratio of compensated to uncompensated midgray "
              "factors), distinct from the comp+normalize branch in "
              "print_portra_evcomp. Together the two cases pin both EV-active branches "
              "of the 4-case selection logic. Spatial+grain off so it is "
              "deterministic/bit-stable. Tested by tests/test_print_evcomp_e2e.cpp.",
    ),
    Case(
        case_id="scan_provia_couplers",
        film_profile="fujifilm_provia_100f",  # POSITIVE film
        print_profile="kodak_portra_endura",
        scan_film=True,                     # scan_film route on a POSITIVE film.
        deactivate_spatial_effects=True,    # pointwise couplers (no diffusion), no halation
        deactivate_stochastic_effects=True,
        grain_active=False,
        dir_couplers_active=True,           # DIR couplers ON -> exercises the
                                            # POSITIVE-film coupler develop path.
        notes="scan_film route on a POSITIVE film (fujifilm_provia_100f) with the DIR "
              "COUPLERS ON (every other golden is negative film; the positive-film "
              "coupler develop is a distinct branch in model/couplers.py: "
              "compute_density_curves_before_dir_couplers / "
              "compute_exposure_correction_dir_couplers use density_silver = "
              "nanmax(density_curves) - density and interpolate the -density curve). "
              "The provia stock also carries a stock-specific DIR-coupler gamma "
              "override in params_builder._apply_film_specifics "
              "(gamma_samelayer_rgb=(0.156,0.104,0.078), matching interlayer terms) "
              "applied AFTER the generic positive default (0.12,0.08,0.06); the native "
              "digest must reproduce that override or the inhibitor matrix is wrong. "
              "Spatial+grain OFF keep it deterministic/bit-stable. Changes "
              "film_density_cmy (couplers) + final_rgb. Tested by "
              "tests/test_provia_couplers_e2e.cpp.",
    ),
    Case(
        case_id="scan_portra_boost",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=True,
        deactivate_spatial_effects=True,    # boost is pointwise, NOT spatial -> stays on
        deactivate_stochastic_effects=True,
        grain_active=False,
        boost_ev=3.0,                       # +3 stops of highlight lift on the brightest px
        boost_range=0.5,                    # mid steepness (a = 28^(1-0.5))
        protect_ev=1.0,                     # protect up to raw_x0 = 0.184 * 2^1 = 0.368
        notes="Highlight boost ON in filming.expose "
              "(utils/numba_boost_hightlights.boost_highlights, called with "
              "midgray=0.184): lifts every raw value above "
              "raw_x0 = clip(0.184*2^protect_ev, 0, max(raw)) by "
              "k*max_raw*(exp(a*dx)-a*dx-1), a=28^(1-boost_range), "
              "k=(2^boost_ev-1)/(exp(a(1-x0))-a(1-x0)-1). It is pointwise + "
              "deterministic and NOT gated by deactivate_spatial_effects "
              "(params_builder.py only zeroes the scatter/halation sigmas, never "
              "boost_ev), so spatial+grain stay OFF and it is bit-stable. Changes the "
              "film taps (film_log_raw/film_density_cmy) and final_rgb. Tested by "
              "tests/test_highlight_boost_e2e.cpp.",
    ),
    Case(
        case_id="scan_portra_lensblur_nohalation",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=True,
        deactivate_spatial_effects=False,   # required: keeps camera.lens_blur_um alive
        deactivate_stochastic_effects=True,  # grain OFF -> deterministic/bit-stable
        grain_active=False,
        lens_blur_um=1650.0,                # SAME non-default lens blur as scan_portra_lensblur
        halation_off=True,                  # zero ONLY the halation/scatter sigmas
        notes="scan_film with the camera lens blur ON (camera.lens_blur_um=1650µm, "
              "IDENTICAL to scan_portra_lensblur) but the HALATION/SCATTER kernels "
              "zeroed (halation_off=True): only film_render.halation.scatter_core_um / "
              "scatter_tail_um / halation_first_sigma_um are forced to (0,0,0) — the "
              "exact three triples the oracle's deactivate_spatial_effects path zeroes "
              "(params_builder.py c1d0e44 L73-75) — while the DIR-coupler spatial "
              "diffusion and the lens blur stay ON. deactivate_spatial_effects is False "
              "(so the lens blur survives digest); because _apply_halation_preset "
              "re-seeds halation_first_sigma_um during digest, the zeroing is applied "
              "AFTER digest_params (gen_goldens digests, zeros, then simulates with "
              "digest_params_first=False). This DECOUPLES the lens-blur spatial pass "
              "from halation: differencing against scan_portra_lensblur (lens blur + "
              "halation together) isolates the halation contribution. Grain off -> "
              "deterministic/bit-stable. Gates the engine change that lets the lens "
              "blur run with halation independently disabled. Tested by "
              "tests/test_spatial_decouple_e2e.cpp.",
    ),
    Case(
        case_id="print_portra_spatial",
        film_profile="kodak_portra_400",
        print_profile="kodak_portra_endura",
        scan_film=False,                    # PRINT route (negative->enlarger->print->scan)
        deactivate_spatial_effects=False,   # spatial branch ON: in-emulsion scatter +
                                            # back-reflection halation + DIR-coupler diffusion
        deactivate_stochastic_effects=True,  # grain + print glare OFF -> deterministic
        grain_active=False,
        notes="PRINT route with the SPATIAL EFFECTS ON "
              "(deactivate_spatial_effects=False: in-emulsion scatter + back-reflection "
              "halation in FilmingStage.expose, DIR-coupler spatial diffusion in "
              "develop) but grain + print glare OFF (deactivate_stochastic_effects="
              "True, grain_active=False), same profile/filters as print_portra. The "
              "scan_portra_spatial golden only covers the spatial branch on the "
              "scan_film route; this pins the SAME spatial branch feeding the PRINT "
              "stage — the spatial halation/scatter carried in film_density_cmy flows "
              "through printing into print_density_cmy + final_rgb. Deterministic/"
              "bit-stable (only grain/glare are stochastic; the halation/scatter/"
              "coupler-diffusion kernels are deterministic convolutions). Gates the "
              "print-route spatial-branch parity. Tested by "
              "tests/test_print_spatial_e2e.cpp.",
    ),
]


def _import_spektrafilm() -> tuple[object, OracleProvenance]:
    """Verify and import the exact clean spektrafilm oracle checkout."""
    try:
        spec = importlib.util.find_spec("spektrafilm")
    except Exception as exc:  # noqa: BLE001 - import hooks may raise arbitrary errors
        spec = None
        find_error = exc
    else:
        find_error = None

    if spec is None or spec.origin is None:
        detail = (
            f"{type(find_error).__name__}: {find_error}"
            if find_error is not None
            else "module could not be resolved to a source file"
        )
        sys.stderr.write(
            "ERROR: could not locate 'spektrafilm'.\n"
            f"  ({detail})\n\n"
            "gen_goldens.py needs a working spektrafilm source checkout to "
            "PRODUCE goldens.\n"
        )
        raise SystemExit(2) from find_error

    try:
        provenance = verify_oracle_provenance(Path(spec.origin), ORACLE_COMMIT)
    except OracleProvenanceError as exc:
        sys.stderr.write(
            "ERROR: refusing unverified spektrafilm oracle checkout.\n"
            f"  {exc}\n"
        )
        raise SystemExit(2) from exc

    previous_dont_write_bytecode = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        import spektrafilm  # noqa: F401
        from spektrafilm import init_params, simulate  # noqa: F401
        imported = sys.modules["spektrafilm"]
    except Exception as exc:  # noqa: BLE001 - upstream can raise asset/runtime errors
        sys.stderr.write(
            "ERROR: could not import 'spektrafilm'.\n"
            f"  ({type(exc).__name__}: {exc})\n\n"
            "gen_goldens.py needs a working spektrafilm environment to PRODUCE\n"
            "goldens. Set it up per the spektrafilm README (clone the repo,\n"
            "install the package and its data assets), then run this script from\n"
            "that environment, e.g.:\n\n"
            "    cd /path/to/spektrafilm && pip install -e .\n"
            "    python /path/to/Spectrafilmandroid/tools/parity/gen_goldens.py\n\n"
            "Reading/inspecting existing .spkvec goldens and building the C++\n"
            "comparator do NOT require spektrafilm.\n"
        )
        raise SystemExit(2) from exc
    finally:
        sys.dont_write_bytecode = previous_dont_write_bytecode

    imported_file = getattr(imported, "__file__", None)
    if imported_file is None:
        sys.stderr.write(
            "ERROR: refusing spektrafilm import without a concrete source file.\n"
        )
        raise SystemExit(2)
    try:
        imported_provenance = verify_oracle_provenance(
            Path(imported_file), ORACLE_COMMIT
        )
    except OracleProvenanceError as exc:
        sys.stderr.write(
            "ERROR: refusing unverified imported spektrafilm oracle checkout.\n"
            f"  {exc}\n"
        )
        raise SystemExit(2) from exc
    if imported_provenance.repository_root != provenance.repository_root:
        sys.stderr.write(
            "ERROR: spektrafilm import resolved from a different checkout after "
            "verification.\n"
            f"  verified: {provenance.repository_root}\n"
            f"  imported: {imported_provenance.repository_root}\n"
        )
        raise SystemExit(2)
    return imported, imported_provenance


def make_test_image(size: int) -> "object":
    """Build a deterministic synthetic linear-RGB test image, no external files.

    Layout (so a single image stresses several pipeline behaviours):
      * a smooth diagonal RGB ramp across the frame (low -> high exposure),
      * a coarse grid of saturated/neutral patches (Macbeth-like) in the center,
      * a few near-black and near-white tiles for shadow/highlight clipping.

    Values are scene-linear in [0, ~1.2] (ProPhoto RGB linear, the spektrafilm
    default input space). Fully deterministic for a given ``size``.
    """
    import numpy as np

    rng = np.random.default_rng(SEED)
    h = w = int(size)
    img = np.zeros((h, w, 3), dtype=np.float64)

    # 1) Diagonal RGB ramp: R increases with x, G with y, B with x+y.
    xx, yy = np.meshgrid(np.linspace(0.0, 1.0, w), np.linspace(0.0, 1.0, h))
    img[..., 0] = xx
    img[..., 1] = yy
    img[..., 2] = 0.5 * (xx + yy)

    # 2) Macbeth-like 6x4 grid of fixed patches in the central region.
    patches = [
        (0.05, 0.05, 0.05), (0.20, 0.20, 0.20), (0.45, 0.45, 0.45), (0.90, 0.90, 0.90),
        (0.55, 0.10, 0.10), (0.10, 0.45, 0.12), (0.10, 0.15, 0.55), (0.70, 0.60, 0.10),
        (0.40, 0.12, 0.45), (0.10, 0.50, 0.55), (0.65, 0.30, 0.12), (0.20, 0.25, 0.65),
        (0.85, 0.30, 0.20), (0.25, 0.55, 0.30), (0.75, 0.15, 0.30), (0.90, 0.75, 0.10),
        (0.55, 0.20, 0.55), (0.15, 0.55, 0.70), (1.10, 1.10, 1.10), (0.01, 0.01, 0.01),
        (0.30, 0.30, 0.10), (0.10, 0.30, 0.30), (0.30, 0.10, 0.30), (0.50, 0.50, 0.20),
    ]
    cols, rows = 6, 4
    gw = max(1, w // 2 // cols)
    gh = max(1, h // 2 // rows)
    y0 = (h - gh * rows) // 2
    x0 = (w - gw * cols) // 2
    for idx, (r, g, b) in enumerate(patches):
        gr, gc = divmod(idx, cols)
        ys, xs = y0 + gr * gh, x0 + gc * gw
        img[ys:ys + gh, xs:xs + gw, 0] = r
        img[ys:ys + gh, xs:xs + gw, 1] = g
        img[ys:ys + gh, xs:xs + gw, 2] = b

    # 3) Tiny deterministic dither so AE/normalization paths see variety, but
    #    bounded so the image stays reproducible and physically sane.
    img += (rng.random((h, w, 3)) - 0.5) * 1e-3
    np.clip(img, 0.0, None, out=img)
    return img.astype(np.float64)


def _build_params(sf, case: Case):
    """Build digested-ready params for a case (returns undigested RuntimePhotoParams)."""
    params = sf.init_params(
        film_profile=case.film_profile,
        print_profile=case.print_profile,
    )
    params.io.scan_film = case.scan_film
    params.io.crop = case.crop
    params.io.crop_center = tuple(case.crop_center)
    params.io.crop_size = tuple(case.crop_size)
    params.io.upscale_factor = case.upscale_factor
    params.debug.deactivate_stochastic_effects = case.deactivate_stochastic_effects
    params.debug.deactivate_spatial_effects = case.deactivate_spatial_effects
    params.film_render.grain.active = case.grain_active
    # Auto-exposure: OFF by default (deterministic — identical inputs map to
    # identical exposures regardless of metering tweaks). A case may turn it ON
    # to exercise the metering stage; the synthetic image is fully deterministic
    # so the metered EV is reproducible.
    params.camera.auto_exposure = case.auto_exposure
    params.camera.auto_exposure_method = case.auto_exposure_method
    params.camera.exposure_compensation_ev = case.exposure_compensation_ev
    # Camera lens blur (µm). Only meaningful when spatial effects are kept on
    # (digest_params zeroes it under deactivate_spatial_effects=True).
    params.camera.lens_blur_um = case.lens_blur_um
    # Camera optical diffusion filter. Only honoured when spatial effects are kept
    # on (digest_params zeroes camera.diffusion_filter.active under
    # deactivate_spatial_effects=True). When active we set a real family + a
    # non-default strength so the filter does visible work end to end.
    params.camera.diffusion_filter.active = case.diffusion_active
    params.camera.diffusion_filter.filter_family = case.diffusion_family
    params.camera.diffusion_filter.strength = case.diffusion_strength
    params.camera.diffusion_filter.spatial_scale = case.diffusion_spatial_scale
    params.camera.diffusion_filter.halo_warmth = case.diffusion_halo_warmth
    # Spectral-domain Gaussian blur of the spectra LUT. Lives in settings, not the
    # spatial branch, so it is honoured regardless of deactivate_spatial_effects.
    params.settings.spectral_gaussian_blur = case.spectral_gaussian_blur
    # Hanatos2025 sensitivity-adaptation toggles. Window defaults ON, surface
    # defaults OFF (schema defaults); a case may flip the surface on to exercise the
    # poly4 log-exposure-correction surface in compute_hanatos2025_tc_lut.
    params.settings.apply_hanatos2025_adaptation_window = case.apply_hanatos_window
    params.settings.apply_hanatos2025_adaptation_surface = case.apply_hanatos_surface
    # Camera UV/IR cut band-pass filter. Lives in the LUT-build path (the band-pass
    # is folded into the sensitivity before compute_hanatos2025_tc_lut), so it is
    # honoured regardless of deactivate_spatial_effects. Defaults (amp 0) no-op.
    params.camera.filter_uv = tuple(case.filter_uv)
    params.camera.filter_ir = tuple(case.filter_ir)
    # Enlarger preflash (print route only). Default exposure 0.0 is a strict no-op.
    params.enlarger.preflash_exposure = case.preflash_exposure
    params.enlarger.preflash_y_filter_shift = case.preflash_y_filter_shift
    params.enlarger.preflash_m_filter_shift = case.preflash_m_filter_shift
    # Scanner black/white corrections. Default OFF (strict no-op).
    params.scanner.black_correction = case.scanner_black_correction
    params.scanner.white_correction = case.scanner_white_correction
    params.scanner.black_level = case.scanner_black_level
    params.scanner.white_level = case.scanner_white_level
    params.film_render.dir_couplers.active = case.dir_couplers_active
    # Highlight boost (film_render.halation.boost_ev/boost_range/protect_ev), applied
    # in filming.expose via boost_highlights right after the exposure-comp scale and
    # before the spatial branch. NOT gated by deactivate_spatial_effects. Default
    # boost_ev 0.0 is a strict no-op.
    params.film_render.halation.boost_ev = case.boost_ev
    params.film_render.halation.boost_range = case.boost_range
    params.film_render.halation.protect_ev = case.protect_ev
    # Enlarger midgray-balance toggles (PRINT route). Both default True (schema).
    params.enlarger.print_exposure_compensation = case.print_exposure_compensation
    params.enlarger.normalize_print_exposure = case.normalize_print_exposure
    return params


def _run_tap(sf, params, tap_name: str, image, halation_off: bool = False):
    """Run simulate once configured for ``tap_name`` and return the buffer."""
    import copy

    p = copy.deepcopy(params)
    field = TAPS[tap_name]
    if field is None:
        # Final RGB: full pipeline, debug off.
        p.debug.debug_mode = "off"
        p.debug.output_film_log_raw = False
        p.debug.output_film_density_cmy = False
        p.debug.output_print_density_cmy = False
    else:
        p.debug.debug_mode = "output"
        p.debug.output_film_log_raw = False
        p.debug.output_film_density_cmy = False
        p.debug.output_print_density_cmy = False
        setattr(p.debug, field, True)
    if halation_off:
        # Zero ONLY the three halation-object spatial sigmas (see Case.halation_off).
        # This MUST run AFTER digest_params: _apply_halation_preset re-seeds
        # halation_first_sigma_um from the profile preset during digest, so we digest
        # explicitly, zero the three triples, then simulate WITHOUT re-digesting. Every
        # other spatial effect (camera.lens_blur_um, DIR-coupler diffusion_size_um) is
        # preserved, decoupling the halation/scatter kernels from the rest of the
        # spatial branch.
        p = sf.digest_params(p)
        p.film_render.halation.scatter_core_um = (0.0, 0.0, 0.0)
        p.film_render.halation.scatter_tail_um = (0.0, 0.0, 0.0)
        p.film_render.halation.halation_first_sigma_um = (0.0, 0.0, 0.0)
        out = sf.simulate(image, p, digest_params_first=False)
    else:
        out = sf.simulate(image, p)  # digests params internally
    import numpy as np

    return np.ascontiguousarray(np.asarray(out, dtype=np.float32))


def generate_case(
    sf, case: Case, size: int, oracle_provenance: OracleProvenance
) -> dict:
    """Generate all taps for one case; write .spkvec files + return manifest dict."""
    import colour
    import spkvec  # local module, see spkvec_format.md

    image = make_test_image(size)
    params = _build_params(sf, case)

    GOLDENS_DIR.mkdir(parents=True, exist_ok=True)
    case_dir = GOLDENS_DIR / case.case_id
    with tempfile.TemporaryDirectory(
        prefix=f".{case.case_id}-stage-", dir=GOLDENS_DIR
    ) as staging_directory:
        staging_dir = Path(staging_directory)
        artifacts: list[tuple[Path, Path]] = []
        written = {}
        for tap_name in case.taps:
            if case.scan_film and tap_name in SCAN_FILM_SKIP_TAPS:
                continue
            buf = _run_tap(
                sf, params, tap_name, image, halation_off=case.halation_off
            )
            filename = f"{tap_name}.spkvec"
            staged_path = staging_dir / filename
            destination = case_dir / filename
            spkvec.write(staged_path, buf)
            artifacts.append((staged_path, destination))
            written[tap_name] = {
                "file": filename,
                "shape": list(buf.shape),
                "min": float(buf.min()),
                "max": float(buf.max()),
                "mean": float(buf.mean()),
            }

        # Keep the direct and accelerated scanner outputs side by side. The LUT
        # is an upstream feature with its own deterministic output; it is not
        # expected to be bit-identical to direct integration at a coarse grid.
        for resolution in case.scanner_lut_resolutions:
            import copy

            lut_params = copy.deepcopy(params)
            lut_params.settings.use_scanner_lut = True
            lut_params.settings.lut_resolution = int(resolution)
            tap_name = f"final_rgb_scanner_lut_{resolution}"
            buf = _run_tap(
                sf,
                lut_params,
                "final_rgb",
                image,
                halation_off=case.halation_off,
            )
            filename = f"{tap_name}.spkvec"
            staged_path = staging_dir / filename
            destination = case_dir / filename
            spkvec.write(staged_path, buf)
            artifacts.append((staged_path, destination))
            written[tap_name] = {
                "file": filename,
                "shape": list(buf.shape),
                "min": float(buf.min()),
                "max": float(buf.max()),
                "mean": float(buf.mean()),
                "lut_resolution": int(resolution),
            }

        manifest = {
            "case_id": case.case_id,
            "film_profile": case.film_profile,
            "print_profile": case.print_profile,
            "scan_film": case.scan_film,
            "image": {
                "kind": "synthetic_ramp_macbeth",
                "size": size,
                "shape": [size, size, 3],
                "input_color_space": "ProPhoto RGB (linear)",
                "seed": SEED,
            },
            "toggles": {
                "auto_exposure": case.auto_exposure,
                "auto_exposure_method": case.auto_exposure_method,
                "exposure_compensation_ev": case.exposure_compensation_ev,
                "print_exposure_compensation": case.print_exposure_compensation,
                "normalize_print_exposure": case.normalize_print_exposure,
                "grain_active": case.grain_active,
                "deactivate_stochastic_effects": case.deactivate_stochastic_effects,
                "deactivate_spatial_effects": case.deactivate_spatial_effects,
                "halation_off": case.halation_off,
                "lens_blur_um": case.lens_blur_um,
                "diffusion_active": case.diffusion_active,
                "diffusion_family": case.diffusion_family,
                "diffusion_strength": case.diffusion_strength,
                "diffusion_spatial_scale": case.diffusion_spatial_scale,
                "diffusion_halo_warmth": case.diffusion_halo_warmth,
                "spectral_gaussian_blur": case.spectral_gaussian_blur,
                "scanner_black_correction": case.scanner_black_correction,
                "scanner_white_correction": case.scanner_white_correction,
                "scanner_black_level": case.scanner_black_level,
                "scanner_white_level": case.scanner_white_level,
                "scanner_lut_resolutions": list(case.scanner_lut_resolutions),
            },
            "notes": case.notes,
            "taps": written,
            # Grain-off / spatial-off cases are deterministic, so these are tight.
            "tolerance": {"max_abs": 1e-4, "rms": 1e-5},
            "spkvec_version": 1,
            "generator": "gen_goldens.py",
            "oracle": {
                "repository": ORACLE_REPOSITORY,
                "colour_version": colour.__version__,
            },
        }
        # Re-check after all oracle execution, before any attested output changes.
        oracle_provenance = verify_oracle_provenance(
            oracle_provenance.repository_root, ORACLE_COMMIT
        )
        manifest["oracle"].update(oracle_provenance.as_manifest())
        staged_manifest = staging_dir / "manifest.json"
        with open(staged_manifest, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(manifest, fh, indent=2, sort_keys=True)

        publish_verified_artifacts(
            artifacts,
            staged_manifest,
            case_dir / "manifest.json",
            oracle_provenance,
        )

    for _, destination in artifacts:
        tap_name = destination.stem
        print(
            f"  wrote {destination.relative_to(HERE)}  "
            f"shape={written[tap_name]['shape']}"
        )
    print(f"  wrote {(case_dir / 'manifest.json').relative_to(HERE)}")
    return manifest


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--case", action="append", default=None,
                        help="case id to generate (repeatable); default = all")
    parser.add_argument("--size", type=int, default=64,
                        help="edge length of the square synthetic test image (px)")
    parser.add_argument("--list", action="store_true",
                        help="list available cases and exit (no spektrafilm needed)")
    args = parser.parse_args(argv)

    by_id = {c.case_id: c for c in CASES}

    if args.list:
        for c in CASES:
            route = "scan_film" if c.scan_film else "print"
            print(f"{c.case_id:16s}  {c.film_profile} / {c.print_profile}  [{route}]")
        return 0

    if args.case:
        unknown = [c for c in args.case if c not in by_id]
        if unknown:
            sys.stderr.write(f"unknown case(s): {', '.join(unknown)}\n")
            return 2
        cases = [by_id[c] for c in args.case]
    else:
        cases = CASES

    sf, oracle_provenance = _import_spektrafilm()
    GOLDENS_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Generating goldens into {GOLDENS_DIR} (image {args.size}x{args.size})")
    for case in cases:
        print(f"[case] {case.case_id}")
        try:
            generate_case(sf, case, args.size, oracle_provenance)
        except OracleProvenanceError as exc:
            sys.stderr.write(
                "ERROR: oracle provenance changed during golden generation; "
                "manifest was not written.\n"
                f"  {exc}\n"
            )
            return 2
    print("Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

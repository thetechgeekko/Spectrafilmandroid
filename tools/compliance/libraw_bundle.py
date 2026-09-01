#!/usr/bin/env python3
"""Build and verify Spektrafilm's deterministic LibRaw compliance bundle.

This tool intentionally has no network client and never discovers a CMake
``build/_deps`` tree.  The caller supplies the official archive explicitly;
its byte length and SHA-256 must match the pins in LibRawVendor.cmake before any
source is accepted.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import tarfile
import tempfile
import zipfile
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from datetime import date, datetime, timezone
from pathlib import Path, PurePosixPath
from urllib.parse import urlsplit

SCHEMA = "spektrafilm.libraw-compliance-bundle/v1"
SPDX_VERSION = "SPDX-2.3"
MANIFEST_PATH = "manifest.json"
SBOM_PATH = "sbom.spdx.json"
ROUTE_REPOSITORY_PATH = Path("lib/libraw/compliance/license-route.txt")
ROUTE_BUNDLE_PATH = "relink/lib/libraw/compliance/license-route.txt"
DECISION_SCHEMA = "spektrafilm.libraw-license-decision/v1"
DECISION_REPOSITORY_PATH = Path("lib/libraw/compliance/license-decision.json")
DECISION_BUNDLE_PATH = "relink/lib/libraw/compliance/license-decision.json"
SPDX_CREATED_REPOSITORY_PATH = Path("lib/libraw/compliance/spdx-created-at.txt")
SPDX_CREATED_BUNDLE_PATH = "relink/lib/libraw/compliance/spdx-created-at.txt"
RESOLVER_REPOSITORY_PATH = Path("lib/libraw/cmake/LibRawVendor.cmake")
RESOLVER_BUNDLE_PATH = "relink/lib/libraw/cmake/LibRawVendor.cmake"
PATCH_REPOSITORY_DIR = Path("lib/libraw/patches")
PATCH_BUNDLE_DIR = PurePosixPath("relink/lib/libraw/patches")
PATCH_README_REPOSITORY_PATH = PATCH_REPOSITORY_DIR / "README.md"
PATCH_README_BUNDLE_PATH = (PATCH_BUNDLE_DIR / "README.md").as_posix()
NATIVE_REPOSITORY_DIR = Path("lib/libraw/src/main/cpp")
NATIVE_BUNDLE_DIR = PurePosixPath("relink/lib/libraw/src/main/cpp")
RELINK_REPOSITORY_DIR = Path("lib/libraw/compliance/relink")
RELINK_BUNDLE_DIR = PurePosixPath("relink/lib/libraw/compliance/relink")
RELINKING_PATH = "RELINKING.md"
MODIFICATIONS_PATH = "MODIFICATIONS.md"
NOTICE_REPOSITORY_PATH = Path("NOTICE.md")
NOTICE_BUNDLE_PATH = "release/NOTICE.md"
PROJECT_LICENSE_REPOSITORY_PATH = Path("LICENSE")
PROJECT_LICENSE_BUNDLE_PATH = "release/LICENSE.GPL-3.0"
LICENSING_REPOSITORY_PATH = Path("docs/LICENSING.md")
LICENSING_BUNDLE_PATH = "release/docs/LICENSING.md"
AUDITED_FILE_COUNT = 100
MODIFIED_FILE_COUNT = 19
MODIFICATION_DATE = "2026-09-01"
MAX_ARCHIVE_FILES = 20_000
MAX_ARCHIVE_BYTES = 512 * 1024 * 1024
MAX_BUNDLE_FILES = 20_000
MAX_BUNDLE_BYTES = 768 * 1024 * 1024
ZIP_TIMESTAMP = (1980, 1, 1, 0, 0, 0)
KNOWN_ROUTES = frozenset({"UNRESOLVED", "LGPL-2.1-only", "CDDL-1.0"})
RELEASE_ROUTES = frozenset({"LGPL-2.1-only"})
ROUTE_MARKER_REPOSITORY_PATHS = (
    Path("docs/LICENSING.md"),
    Path("docs/dependencies/LIBRAW.md"),
    Path("NOTICE.md"),
    Path("lib/libraw/README.md"),
    Path("lib/libraw/patches/README.md"),
    Path("app/src/main/assets/legal/spektrafilm/NOTICE.md"),
)
APP_NOTICE_ASSET_REPOSITORY_PATH = Path(
    "app/src/main/assets/legal/spektrafilm/NOTICE.md"
)
APP_LICENSE_ASSET_REPOSITORY_PATH = Path(
    "app/src/main/assets/legal/spektrafilm/LICENSE.GPL-3.0"
)
ROUTE_PROSE_REPOSITORY_PATHS = (
    Path("docs/PRODUCTION_READINESS_PLAN.md"),
    Path("docs/RELEASE_CHECKLIST.md"),
    Path("lib/libraw/src/main/cpp/CMakeLists.txt"),
    Path("lib/libraw/src/main/cpp/raw_decoder.cpp"),
    Path("lib/libraw/src/main/cpp/raw_decoder.h"),
    Path("lib/libraw/src/main/cpp/raw_decoder_jni.cpp"),
    Path("lib/libraw/src/main/kotlin/com/spectrafilm/libraw/RawDecoder.kt"),
    Path("lib/libraw/src/main/kotlin/com/spectrafilm/libraw/RawCoilDecoder.kt"),
)
ABOUT_SCREEN_REPOSITORY_PATH = Path(
    "app/src/main/java/com/spectrafilm/app/AboutScreen.kt"
)
ABOUT_TEST_REPOSITORY_PATH = Path(
    "app/src/test/java/com/spectrafilm/app/AboutLegalNoticeTest.kt"
)
MARKDOWN_ROUTE_MARKER = re.compile(
    r"<!-- libraw-license-route: (UNRESOLVED|LGPL-2\.1-only|CDDL-1\.0) -->"
)
KOTLIN_ROUTE_MARKER = re.compile(
    r'internal\s+const\s+val\s+LIBRAW_DISTRIBUTION_ROUTE\s*=\s*'
    r'"(UNRESOLVED|LGPL-2\.1-only|CDDL-1\.0)"'
)
KOTLIN_TEST_ROUTE_MARKER = re.compile(
    r'assertEquals\(\s*"(UNRESOLVED|LGPL-2\.1-only|CDDL-1\.0)"\s*,\s*'
    r'LIBRAW_DISTRIBUTION_ROUTE\s*\)'
)
STALE_UNRESOLVED_PROSE = re.compile(
    r"\bunresolved\b|\bnot\s+(?:yet\s+)?selected\b|"
    r"\bhas\s+not\s+been\s+selected\b|\bdoes\s+not\s+elect\b",
    re.IGNORECASE,
)
HUMAN_ROUTE_ASSERTION = re.compile(
    r"\b(?:selected\s+)?LibRaw(?:\s+Android)?\s+distribution\s+route\s*"
    r"(?:is|:)\s*`?(UNRESOLVED|LGPL-2\.1-only|CDDL-1\.0)`?\.?",
    re.IGNORECASE,
)
REQUIRED_NATIVE_FILES = (
    "CMakeLists.txt",
    "native_allocation_registry.cpp",
    "native_allocation_registry.h",
    "raw_decoder.cpp",
    "raw_decoder.h",
    "raw_decoder_jni.cpp",
    "raw_decoder_jni_safety.h",
    "raw_result_publication.cpp",
    "raw_result_publication.h",
)
REQUIRED_RELINK_FILES = (
    "CMakeLists.txt",
    "README.md",
    "unofficial_relink_marker.cpp",
)
REQUIRED_UPSTREAM_FILES = (
    "COPYRIGHT",
    "LICENSE.LGPL",
    "LICENSE.CDDL",
    "libraw/libraw.h",
)


class BundleError(RuntimeError):
    """A fail-closed provenance, format, or verification error."""


@dataclass(frozen=True)
class Pins:
    version: str
    url: str
    archive_sha256: str
    archive_size: int
    tag_object: str
    commit: str
    patched_tree_sha256: str
    patches: tuple[str, ...]
    patch_sha256: Mapping[str, str]


@dataclass(frozen=True)
class LicenseDecision:
    route: str
    recorded_at: str
    decision_owner: str | None
    decision_date: str | None
    rationale: str | None
    approval_reference: str | None
    patch_license: str | None
    patch_rights_confirmed: bool
    patch_authorization_reference: str | None
    canonical_bytes: bytes


@dataclass(frozen=True)
class Hunk:
    old_start: int
    old_count: int
    new_start: int
    new_count: int
    operations: tuple[tuple[bytes, bytes], ...]


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _canonical_json(value: object) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def _validate_digest(value: str, description: str, length: int = 64) -> None:
    if not re.fullmatch(rf"[0-9a-f]{{{length}}}", value):
        raise BundleError(f"{description} is not a lowercase {length}-hex digest")


def _extract_single_quoted_set(text: str, variable: str) -> str:
    pattern = re.compile(
        rf"set\(\s*{re.escape(variable)}\s*\"([^\"]+)\"\s*\)", re.DOTALL
    )
    matches = pattern.findall(text)
    if len(matches) != 1:
        raise BundleError(
            f"expected exactly one quoted CMake set({variable} ...), found {len(matches)}"
        )
    return matches[0]


def _load_pins(repo_root: Path) -> Pins:
    resolver = repo_root / RESOLVER_REPOSITORY_PATH
    try:
        text = resolver.read_text(encoding="utf-8")
    except OSError as error:
        raise BundleError(f"cannot read pinned resolver {resolver}: {error}") from error

    version = _extract_single_quoted_set(text, "SFRAW_PINNED_LIBRAW_VERSION")
    if not re.fullmatch(r"[0-9A-Za-z][0-9A-Za-z.-]*", version):
        raise BundleError(f"unsafe pinned LibRaw version: {version!r}")
    url = _extract_single_quoted_set(text, "SFRAW_PINNED_LIBRAW_URL")
    if not url.startswith("https://"):
        raise BundleError("pinned LibRaw URL must use HTTPS")
    archive_sha256 = _extract_single_quoted_set(text, "SFRAW_PINNED_LIBRAW_SHA256")
    _validate_digest(archive_sha256, "pinned LibRaw archive SHA-256")
    archive_size_text = _extract_single_quoted_set(
        text, "SFRAW_PINNED_LIBRAW_ARCHIVE_SIZE"
    )
    if not archive_size_text.isdecimal() or int(archive_size_text) <= 0:
        raise BundleError("pinned LibRaw archive size must be a positive integer")
    archive_size = int(archive_size_text)
    tag_object = _extract_single_quoted_set(text, "SFRAW_PINNED_LIBRAW_TAG_OBJECT")
    commit = _extract_single_quoted_set(text, "SFRAW_PINNED_LIBRAW_COMMIT")
    _validate_digest(tag_object, "pinned LibRaw tag object", length=40)
    _validate_digest(commit, "pinned LibRaw commit", length=40)
    patched_tree_sha256 = _extract_single_quoted_set(
        text, "_SFRAW_LIBRAW_PATCHED_TREE_SHA256"
    )
    _validate_digest(patched_tree_sha256, "pinned patched-tree SHA-256")

    list_match = re.search(
        r"set\(\s*_SFRAW_LIBRAW_PATCHES(?P<body>.*?)\)", text, re.DOTALL
    )
    if list_match is None:
        raise BundleError("missing ordered _SFRAW_LIBRAW_PATCHES manifest")
    patches = tuple(
        re.findall(
            r'\"\$\{_SFRAW_LIBRAW_PATCH_DIR\}/([^\"]+)\"',
            list_match.group("body"),
        )
    )
    if not patches or len(patches) != len(set(patches)):
        raise BundleError("ordered LibRaw patch manifest is empty or contains duplicates")
    for patch in patches:
        _validate_safe_relative_path(patch, "patch manifest path")

    pin_pattern = re.compile(
        r'_sfraw_verify_file_sha256\(\s*'
        r'\"\$\{_SFRAW_LIBRAW_PATCH_DIR\}/([^\"]+)\"\s*'
        r'\"([0-9a-f]{64})\"',
        re.DOTALL,
    )
    patch_sha256: dict[str, str] = {}
    for patch, digest in pin_pattern.findall(text):
        if patch in patch_sha256:
            raise BundleError(f"duplicate pinned SHA-256 for patch {patch}")
        patch_sha256[patch] = digest
    if set(patch_sha256) != set(patches):
        missing = sorted(set(patches) - set(patch_sha256))
        extra = sorted(set(patch_sha256) - set(patches))
        raise BundleError(
            f"patch SHA pins disagree with ordered manifest; missing={missing}, extra={extra}"
        )

    return Pins(
        version=version,
        url=url,
        archive_sha256=archive_sha256,
        archive_size=archive_size,
        tag_object=tag_object,
        commit=commit,
        patched_tree_sha256=patched_tree_sha256,
        patches=patches,
        patch_sha256=patch_sha256,
    )


def _load_route(path: Path) -> str:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise BundleError(f"cannot read license route {path}: {error}") from error
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError as error:
        raise BundleError("license route must be ASCII") from error
    lines = text.splitlines()
    if len(lines) != 1 or lines[0] not in KNOWN_ROUTES:
        choices = ", ".join(sorted(KNOWN_ROUTES))
        raise BundleError(f"license route must be exactly one of: {choices}")
    return lines[0]


def _load_spdx_created_at(path: Path) -> str:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise BundleError(f"cannot read SPDX source timestamp {path}: {error}") from error
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError as error:
        raise BundleError("SPDX source timestamp must be ASCII") from error
    lines = text.splitlines()
    if len(lines) != 1 or data != (lines[0] + "\n").encode("ascii"):
        raise BundleError("SPDX source timestamp must be one canonical newline-terminated line")
    _parse_canonical_utc_timestamp(lines[0], "SPDX source timestamp")
    return lines[0]


def _nullable_string(value: object, description: str) -> str | None:
    if value is None:
        return None
    if not isinstance(value, str):
        raise BundleError(f"{description} must be a JSON string or null")
    if value != value.strip() or not value:
        raise BundleError(f"{description} must be a non-empty trimmed string or null")
    return value


def _parse_canonical_utc_timestamp(value: object, description: str) -> datetime:
    if not isinstance(value, str) or not re.fullmatch(
        r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", value
    ):
        raise BundleError(
            f"{description} must use literal YYYY-MM-DDTHH:MM:SSZ"
        )
    try:
        parsed = datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ").replace(
            tzinfo=timezone.utc
        )
    except ValueError as error:
        raise BundleError(
            f"{description} must be a valid UTC timestamp"
        ) from error
    if parsed.strftime("%Y-%m-%dT%H:%M:%SZ") != value:
        raise BundleError(f"{description} is not canonical")
    if parsed > datetime.now(timezone.utc):
        raise BundleError(f"{description} must not be in the future")
    return parsed


def _load_decision(path: Path) -> LicenseDecision:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise BundleError(f"cannot read LibRaw license decision {path}: {error}") from error
    return _parse_decision(data, f"LibRaw license decision {path}")


def _parse_decision(data: bytes, description: str) -> LicenseDecision:
    try:
        value = json.loads(data)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise BundleError(f"{description} is not valid UTF-8 JSON: {error}") from error
    record = _expect_dict(value, description)
    expected_keys = {
        "approval_reference",
        "decision_date",
        "decision_owner",
        "patch_contributions",
        "rationale",
        "recorded_at",
        "route",
        "schema",
    }
    if set(record) != expected_keys:
        raise BundleError(
            "LibRaw license decision has unexpected fields; "
            f"expected={sorted(expected_keys)}, got={sorted(record)}"
        )
    if data != _canonical_json(record):
        raise BundleError("LibRaw license decision is not in canonical deterministic form")
    if record.get("schema") != DECISION_SCHEMA:
        raise BundleError(
            f"unsupported LibRaw license decision schema: {record.get('schema')!r}"
        )
    route = record.get("route")
    if not isinstance(route, str) or route not in KNOWN_ROUTES:
        raise BundleError("LibRaw license decision contains an unknown route")
    recorded_at = record.get("recorded_at")
    _parse_canonical_utc_timestamp(
        recorded_at, "LibRaw license decision recorded_at"
    )
    patch = _expect_dict(
        record.get("patch_contributions"),
        "LibRaw license decision patch_contributions",
    )
    expected_patch_keys = {
        "authorization_reference",
        "license",
        "rights_confirmed",
    }
    if set(patch) != expected_patch_keys:
        raise BundleError(
            "LibRaw patch-contribution decision has unexpected fields; "
            f"expected={sorted(expected_patch_keys)}, got={sorted(patch)}"
        )
    rights_confirmed = patch.get("rights_confirmed")
    if not isinstance(rights_confirmed, bool):
        raise BundleError("LibRaw patch-contribution rights_confirmed must be boolean")
    return LicenseDecision(
        route=route,
        recorded_at=recorded_at,
        decision_owner=_nullable_string(record.get("decision_owner"), "decision_owner"),
        decision_date=_nullable_string(record.get("decision_date"), "decision_date"),
        rationale=_nullable_string(record.get("rationale"), "rationale"),
        approval_reference=_nullable_string(
            record.get("approval_reference"), "approval_reference"
        ),
        patch_license=_nullable_string(patch.get("license"), "patch contribution license"),
        patch_rights_confirmed=rights_confirmed,
        patch_authorization_reference=_nullable_string(
            patch.get("authorization_reference"),
            "patch contribution authorization_reference",
        ),
        canonical_bytes=data,
    )


def _required_human_value(value: str | None, description: str, minimum: int = 3) -> str:
    if value is None or len(value) < minimum:
        raise BundleError(f"resolved LibRaw release requires explicit {description}")
    normalized = value.casefold()
    placeholders = ("pending", "tbd", "todo", "unknown", "unresolved", "n/a")
    if any(token in normalized for token in placeholders):
        raise BundleError(f"resolved LibRaw release rejects placeholder {description}")
    return value


def _required_https_reference(value: str | None, description: str) -> str:
    reference = _required_human_value(value, description)
    parsed = urlsplit(reference)
    if (
        parsed.scheme != "https"
        or not parsed.netloc
        or parsed.username is not None
        or parsed.password is not None
        or any(character.isspace() for character in reference)
    ):
        raise BundleError(f"resolved LibRaw {description} must be an HTTPS URL")
    return reference


def _require_clean_pending_decision(decision: LicenseDecision) -> None:
    pending_values = (
        decision.decision_owner,
        decision.decision_date,
        decision.rationale,
        decision.approval_reference,
        decision.patch_license,
        decision.patch_authorization_reference,
    )
    if any(value is not None for value in pending_values) or decision.patch_rights_confirmed:
        raise BundleError(
            f"LibRaw route {decision.route} requires a clean pending decision record; "
            "approval and patch-authorization fields must remain null/false"
        )


def _require_selected_decision(decision: LicenseDecision) -> None:
    if decision.route == "UNRESOLVED":
        raise BundleError("UNRESOLVED is not a selected LibRaw license route")
    _required_human_value(decision.decision_owner, "decision owner")
    decision_date = _required_human_value(decision.decision_date, "decision date")
    try:
        parsed_date = date.fromisoformat(decision_date)
    except ValueError as error:
        raise BundleError("resolved LibRaw decision date must use YYYY-MM-DD") from error
    if parsed_date.isoformat() != decision_date:
        raise BundleError("resolved LibRaw decision date must use canonical YYYY-MM-DD")
    _required_human_value(decision.rationale, "decision rationale", minimum=20)
    _required_https_reference(decision.approval_reference, "approval reference")
    if decision.patch_license != decision.route:
        raise BundleError(
            "selected LibRaw route requires patch contributions licensed under "
            f"the matching route {decision.route}"
        )
    if not decision.patch_rights_confirmed:
        raise BundleError(
            "selected LibRaw route requires explicit patch-contribution rights confirmation"
        )
    _required_https_reference(
        decision.patch_authorization_reference,
        "patch-contribution authorization reference",
    )
    recorded_date = _parse_canonical_utc_timestamp(
        decision.recorded_at, "LibRaw license decision recorded_at"
    ).date()
    if recorded_date < parsed_date:
        raise BundleError(
            "LibRaw decision recorded_at date must not precede decision_date"
        )


def _require_release_decision(decision: LicenseDecision) -> None:
    route = decision.route
    if route == "CDDL-1.0":
        raise BundleError(
            "license route CDDL-1.0 is a known upstream offer but is not accepted "
            "for release while LibRaw is statically combined with GPL application code; "
            "--require-resolved permits only LGPL-2.1-only"
        )
    if route not in RELEASE_ROUTES:
        raise BundleError(
            "license route is UNRESOLVED; --require-resolved permits only LGPL-2.1-only"
        )
    _require_selected_decision(decision)


def _decode_repository_utf8(repo_root: Path, relative: Path) -> str:
    path = repo_root / relative
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise BundleError(f"cannot read UTF-8 route marker file {path}: {error}") from error


def _single_marker(pattern: re.Pattern[str], text: str, relative: Path) -> str:
    matches = pattern.findall(text)
    if len(matches) != 1:
        raise BundleError(
            f"expected exactly one machine-readable LibRaw route marker in {relative}; "
            f"found {len(matches)}"
        )
    return matches[0]


def _normalized_legal_text(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n").rstrip("\n")


def _verify_route_markers(
    repo_root: Path, route: str, reject_unresolved_prose: bool
) -> None:
    texts: dict[Path, str] = {}
    extracted: dict[Path, str] = {}
    for relative in ROUTE_MARKER_REPOSITORY_PATHS:
        text = _decode_repository_utf8(repo_root, relative)
        texts[relative] = text
        extracted[relative] = _single_marker(MARKDOWN_ROUTE_MARKER, text, relative)
    for relative, pattern in (
        (ABOUT_SCREEN_REPOSITORY_PATH, KOTLIN_ROUTE_MARKER),
        (ABOUT_TEST_REPOSITORY_PATH, KOTLIN_TEST_ROUTE_MARKER),
    ):
        text = _decode_repository_utf8(repo_root, relative)
        texts[relative] = text
        extracted[relative] = _single_marker(pattern, text, relative)
    mismatches = {
        relative.as_posix(): marker
        for relative, marker in extracted.items()
        if marker != route
    }
    if mismatches:
        raise BundleError(
            "LibRaw cross-file route markers differ from canonical license-route.txt: "
            f"expected={route}, mismatches={mismatches}"
        )

    human_claim_sources = {
        relative: texts[relative] for relative in ROUTE_MARKER_REPOSITORY_PATHS
    }
    human_claim_sources[ABOUT_SCREEN_REPOSITORY_PATH] = texts[
        ABOUT_SCREEN_REPOSITORY_PATH
    ]
    missing_claims: list[str] = []
    conflicting_claims: dict[str, list[str]] = {}
    for relative, text in human_claim_sources.items():
        claims = HUMAN_ROUTE_ASSERTION.findall(text)
        if not claims:
            missing_claims.append(relative.as_posix())
            continue
        conflicts = sorted({claim for claim in claims if claim != route})
        if conflicts:
            conflicting_claims[relative.as_posix()] = conflicts
    if missing_claims or conflicting_claims:
        raise BundleError(
            "human-facing LibRaw route claim is missing or contradicts the canonical route: "
            f"expected={route}, missing={missing_claims}, conflicts={conflicting_claims}"
        )

    root_notice = texts[NOTICE_REPOSITORY_PATH]
    app_notice = texts[APP_NOTICE_ASSET_REPOSITORY_PATH]
    if _normalized_legal_text(root_notice) != _normalized_legal_text(app_notice):
        raise BundleError(
            "offline app NOTICE asset differs from normalized root NOTICE.md"
        )
    root_license = _decode_repository_utf8(repo_root, PROJECT_LICENSE_REPOSITORY_PATH)
    app_license = _decode_repository_utf8(repo_root, APP_LICENSE_ASSET_REPOSITORY_PATH)
    if _normalized_legal_text(root_license) != _normalized_legal_text(app_license):
        raise BundleError(
            "offline app GPL asset differs from normalized root LICENSE"
        )

    if reject_unresolved_prose and route != "UNRESOLVED":
        for relative in ROUTE_PROSE_REPOSITORY_PATHS:
            texts[relative] = _decode_repository_utf8(repo_root, relative)
        stale = [
            relative.as_posix()
            for relative, text in texts.items()
            if STALE_UNRESOLVED_PROSE.search(text)
        ]
        if stale:
            raise BundleError(
                "resolved LibRaw release still contains stale unresolved-route prose: "
                f"{stale}"
            )


def _audit_route_state(repo_root: Path, require_resolved: bool) -> LicenseDecision:
    spdx_created_at = _load_spdx_created_at(repo_root / SPDX_CREATED_REPOSITORY_PATH)
    route = _load_route(repo_root / ROUTE_REPOSITORY_PATH)
    decision = _load_decision(repo_root / DECISION_REPOSITORY_PATH)
    if _parse_canonical_utc_timestamp(
        spdx_created_at, "SPDX source timestamp"
    ) < _parse_canonical_utc_timestamp(
        decision.recorded_at, "LibRaw license decision recorded_at"
    ):
        raise BundleError(
            "SPDX source timestamp must not precede the license decision record"
        )
    if decision.route != route:
        raise BundleError(
            "LibRaw license decision route does not match canonical license-route.txt"
        )
    if route == "UNRESOLVED":
        _require_clean_pending_decision(decision)
    else:
        _require_selected_decision(decision)
    _verify_route_markers(
        repo_root,
        route,
        reject_unresolved_prose=route != "UNRESOLVED" or require_resolved,
    )
    if require_resolved:
        _require_release_decision(decision)
    return decision


def _validate_safe_relative_path(path: str, description: str) -> PurePosixPath:
    if "\\" in path or "\x00" in path:
        raise BundleError(f"unsafe {description}: {path!r}")
    pure = PurePosixPath(path)
    if pure.is_absolute() or not pure.parts or any(
        part in ("", ".", "..") for part in pure.parts
    ):
        raise BundleError(f"unsafe {description}: {path!r}")
    return pure


def _verify_archive(archive: Path, pins: Pins) -> None:
    try:
        size = archive.stat().st_size
    except OSError as error:
        raise BundleError(f"cannot stat supplied LibRaw archive {archive}: {error}") from error
    if size != pins.archive_size:
        raise BundleError(
            f"LibRaw archive size mismatch: expected {pins.archive_size}, got {size}"
        )
    digest = _file_sha256(archive)
    if digest != pins.archive_sha256:
        raise BundleError(
            f"LibRaw archive SHA-256 mismatch: expected {pins.archive_sha256}, got {digest}"
        )


def _extract_archive(archive: Path, destination: Path, pins: Pins) -> Path:
    expected_root = f"LibRaw-{pins.version}"
    seen_files: set[str] = set()
    total_bytes = 0
    member_count = 0
    try:
        source = tarfile.open(archive, mode="r:gz")  # noqa: SIM115 - closed below
    except (OSError, tarfile.TarError) as error:
        raise BundleError(f"cannot open authenticated LibRaw archive: {error}") from error

    with source:
        for member in source:
            member_count += 1
            if member_count > MAX_ARCHIVE_FILES:
                raise BundleError("LibRaw archive exceeds the file-count safety limit")
            normalized_name = member.name.rstrip("/")
            pure = _validate_safe_relative_path(normalized_name, "archive member path")
            if pure.parts[0] != expected_root:
                raise BundleError(
                    f"archive member is outside expected {expected_root}/ root: {member.name}"
                )
            if member.isdir():
                continue
            if not member.isfile():
                raise BundleError(
                    f"archive contains unsupported link or special member: {member.name}"
                )
            relative_parts = pure.parts[1:]
            if not relative_parts:
                raise BundleError("archive root cannot be a regular file")
            relative = PurePosixPath(*relative_parts).as_posix()
            if relative in seen_files:
                raise BundleError(f"archive contains duplicate file: {relative}")
            seen_files.add(relative)
            total_bytes += member.size
            if member.size < 0 or total_bytes > MAX_ARCHIVE_BYTES:
                raise BundleError("LibRaw archive exceeds the extracted-byte safety limit")
            extracted = source.extractfile(member)
            if extracted is None:
                raise BundleError(f"cannot read archive member: {member.name}")
            data = extracted.read(member.size + 1)
            if len(data) != member.size:
                raise BundleError(f"archive member size mismatch: {member.name}")
            target = destination.joinpath(*relative_parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    if not seen_files:
        raise BundleError("authenticated LibRaw archive contains no files")
    return destination


_HUNK_HEADER = re.compile(
    rb"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(?:.*)(?:\r?\n)?$"
)


def _header_path(line: bytes, prefix: bytes) -> str:
    if not line.startswith(prefix):
        raise BundleError("malformed unified patch file header")
    value = line[len(prefix) :].rstrip(b"\r\n").split(b"\t", 1)[0]
    try:
        text = value.decode("utf-8")
    except UnicodeDecodeError as error:
        raise BundleError("patch path is not UTF-8") from error
    if text == "/dev/null" or not text.startswith(("a/", "b/")):
        raise BundleError(f"unsupported unified patch path: {text!r}")
    safe = _validate_safe_relative_path(text[2:], "patch target path")
    return safe.as_posix()


def _parse_hunk(lines: Sequence[bytes], index: int) -> tuple[Hunk, int]:
    match = _HUNK_HEADER.match(lines[index])
    if match is None:
        raise BundleError("malformed unified patch hunk header")
    old_start = int(match.group(1))
    old_count = int(match.group(2) or b"1")
    new_start = int(match.group(3))
    new_count = int(match.group(4) or b"1")
    old_used = 0
    new_used = 0
    operations: list[tuple[bytes, bytes]] = []
    index += 1
    while old_used < old_count or new_used < new_count:
        if index >= len(lines):
            raise BundleError("unified patch hunk ended before its declared line counts")
        line = lines[index]
        if not line or line[:1] not in (b" ", b"-", b"+"):
            raise BundleError("invalid line inside unified patch hunk")
        operation = line[:1]
        payload = line[1:]
        operations.append((operation, payload))
        if operation in (b" ", b"-"):
            old_used += 1
        if operation in (b" ", b"+"):
            new_used += 1
        if old_used > old_count or new_used > new_count:
            raise BundleError("unified patch hunk exceeds its declared line counts")
        index += 1
    if index < len(lines) and lines[index].startswith(b"\\ No newline at end of file"):
        if not operations:
            raise BundleError("orphan no-newline marker in unified patch")
        operation, payload = operations[-1]
        operations[-1] = (operation, payload.rstrip(b"\r\n"))
        index += 1
    return (
        Hunk(old_start, old_count, new_start, new_count, tuple(operations)),
        index,
    )


def _parse_patch(data: bytes) -> tuple[tuple[str, tuple[Hunk, ...]], ...]:
    lines = data.splitlines(keepends=True)
    index = 0
    files: list[tuple[str, tuple[Hunk, ...]]] = []
    while index < len(lines):
        if not lines[index].startswith(b"--- "):
            index += 1
            continue
        old_path = _header_path(lines[index], b"--- ")
        index += 1
        if index >= len(lines):
            raise BundleError("unified patch is missing +++ file header")
        new_path = _header_path(lines[index], b"+++ ")
        index += 1
        if old_path != new_path:
            raise BundleError("file creation, deletion, and rename patches are unsupported")
        hunks: list[Hunk] = []
        while index < len(lines):
            if lines[index].startswith(b"@@ "):
                hunk, index = _parse_hunk(lines, index)
                hunks.append(hunk)
                continue
            if lines[index].startswith(b"--- ") or lines[index].startswith(b"diff --git "):
                break
            if lines[index].strip():
                raise BundleError(
                    f"unexpected content after hunks for patch target {old_path}"
                )
            index += 1
        if not hunks:
            raise BundleError(f"patch target has no hunks: {old_path}")
        files.append((old_path, tuple(hunks)))
    if not files:
        raise BundleError("patch contains no supported file hunks")
    return tuple(files)


def _apply_hunks(original: bytes, hunks: Sequence[Hunk], path: str) -> bytes:
    current_lines = original.splitlines(keepends=True)
    minimum_target = 0
    cumulative_actual_delta = 0
    prior_context_offset = 0
    for hunk in hunks:
        declared_target = hunk.old_start - 1 if hunk.old_count else hunk.old_start

        old_sequence = [
            payload for operation, payload in hunk.operations if operation in (b" ", b"-")
        ]
        new_sequence = [
            payload for operation, payload in hunk.operations if operation in (b" ", b"+")
        ]
        if len(old_sequence) != hunk.old_count or len(new_sequence) != hunk.new_count:
            raise BundleError(f"patch hunk line counts are inconsistent for {path}")
        if not old_sequence:
            raise BundleError(
                f"zero-context insertion hunks are unsupported for deterministic patching: {path}"
            )

        candidate_count = len(current_lines) - len(old_sequence) + 1
        candidates = [
            index
            for index in range(max(candidate_count, 0))
            if index >= minimum_target
            and current_lines[index : index + len(old_sequence)] == old_sequence
        ]
        if not candidates:
            raise BundleError(
                f"patch has no exact old-context match in authenticated source: {path}"
            )
        predicted_target = (
            declared_target + cumulative_actual_delta + prior_context_offset
        )
        smallest_distance = min(
            abs(candidate - predicted_target) for candidate in candidates
        )
        nearest = [
            candidate
            for candidate in candidates
            if abs(candidate - predicted_target) == smallest_distance
        ]
        if len(nearest) != 1:
            raise BundleError(
                "patch old-context match has an equal-distance ambiguity "
                f"({len(nearest)} nearest of {len(candidates)} exact matches): {path}"
            )
        target = nearest[0]

        # The target includes shifts made by earlier ordered patches and by
        # earlier hunks in this file. Exact-context matching permits that offset,
        # while cumulative_actual_delta keeps later hunk coordinates coherent.
        prior_context_offset = target - (
            declared_target + cumulative_actual_delta
        )
        current_lines[target : target + len(old_sequence)] = new_sequence
        minimum_target = target + len(new_sequence)
        line_delta = hunk.new_count - hunk.old_count
        cumulative_actual_delta += line_delta
    return b"".join(current_lines)


def _apply_patch(source_root: Path, patch_path: Path) -> None:
    try:
        parsed = _parse_patch(patch_path.read_bytes())
    except BundleError as error:
        raise BundleError(f"cannot parse pinned patch {patch_path.name}: {error}") from error
    except OSError as error:
        raise BundleError(f"cannot read patch {patch_path}: {error}") from error
    for relative, hunks in parsed:
        target = source_root.joinpath(*PurePosixPath(relative).parts)
        if not target.is_file():
            raise BundleError(
                f"cannot apply pinned patch {patch_path.name}: target is absent from "
                f"authenticated source: {relative}"
            )
        try:
            patched = _apply_hunks(target.read_bytes(), hunks, relative)
        except BundleError as error:
            raise BundleError(f"cannot apply pinned patch {patch_path.name}: {error}") from error
        target.write_bytes(patched)


def _audited_source_files_from_disk(source_root: Path) -> dict[str, bytes]:
    audited: dict[str, bytes] = {}
    for path in source_root.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(source_root).as_posix()
        if (
            (relative.startswith("src/") and relative.endswith(".cpp"))
            or (relative.startswith("libraw/") and relative.endswith(".h"))
            or (relative.startswith("internal/") and relative.endswith(".h"))
        ):
            audited[relative] = path.read_bytes()
    return audited


def _tree_aggregate(audited: Mapping[str, bytes]) -> str:
    manifest = "".join(
        f"{path}:{_sha256(audited[path])}\n" for path in sorted(audited)
    ).encode("ascii")
    return _sha256(manifest)


def _verify_patched_source(source_root: Path, pins: Pins) -> None:
    audited = _audited_source_files_from_disk(source_root)
    if len(audited) != AUDITED_FILE_COUNT:
        raise BundleError(
            f"patched LibRaw tree has {len(audited)} audited files; "
            f"expected {AUDITED_FILE_COUNT}"
        )
    aggregate = _tree_aggregate(audited)
    if aggregate != pins.patched_tree_sha256:
        raise BundleError(
            f"patched LibRaw aggregate mismatch: expected {pins.patched_tree_sha256}, "
            f"got {aggregate}"
        )
    for relative in REQUIRED_UPSTREAM_FILES:
        if not (source_root / relative).is_file():
            raise BundleError(f"patched source is missing required file: {relative}")


def _relinking_text(version: str, route: str) -> bytes:
    text = f"""# Relinking the bundled LibRaw source

This archive contains the authenticated, fully patched LibRaw {version} source
used by Spektrafilm plus the native `libsfraw.so` wrapper source and build files.
Its recorded license route is `{route}`. This record is build evidence, not an
independent legal approval; release verification accepts only `LGPL-2.1-only`.

Set `ANDROID_NDK_HOME` to Android NDK 27.0.12077973, then build an ABI-compatible
replacement with CMake 3.22.1 or newer. The standalone recipient project takes
the source explicitly and deliberately does not apply the production aggregate
gate, so a modified, interface-compatible LibRaw tree can be relinked.

```sh
cmake -S relink/lib/libraw/compliance/relink -B out/arm64-v8a -G Ninja \\
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \\
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \\
  -DANDROID_STL=c++_shared \\
  -DCMAKE_BUILD_TYPE=Release \\
  -DLIBRAW_SOURCE_DIR="$PWD/source/LibRaw-{version}"
cmake --build out/arm64-v8a --parallel
```

Repeat with `armeabi-v7a` or `x86_64` as needed. Repackage the resulting
`libsfraw.so` into a source-built APK and sign that APK with your own key; the
production signing key is neither necessary nor included in this bundle.
"""
    return text.encode("utf-8")


def _sha1(data: bytes) -> str:
    return hashlib.sha1(data, usedforsecurity=False).hexdigest()


def _spdx_file_id(path: str) -> str:
    return f"SPDXRef-File-{_sha256(path.encode('utf-8'))[:24]}"


def _spdx_file_record(path: str, data: bytes, concluded: str = "NOASSERTION") -> dict[str, object]:
    return {
        "SPDXID": _spdx_file_id(path),
        "checksums": [
            {"algorithm": "SHA1", "checksumValue": _sha1(data)},
            {"algorithm": "SHA256", "checksumValue": _sha256(data)},
        ],
        "copyrightText": "NOASSERTION",
        "fileName": path,
        "licenseConcluded": concluded,
        "licenseInfoInFiles": ["NOASSERTION"],
    }


def _package_verification_code(payload: Mapping[str, bytes], paths: Sequence[str]) -> str:
    concatenated = "".join(sorted(_sha1(payload[path]) for path in paths)).encode("ascii")
    return _sha1(concatenated)


def _spdx_source_paths(
    pins: Pins, payload: Mapping[str, bytes]
) -> tuple[tuple[str, ...], tuple[str, ...], tuple[str, ...]]:
    source_prefix = f"source/LibRaw-{pins.version}/"
    patched_paths = tuple(sorted(path for path in payload if path.startswith(source_prefix)))
    wrapper_paths = tuple(
        sorted(
            [(NATIVE_BUNDLE_DIR / name).as_posix() for name in REQUIRED_NATIVE_FILES]
            + [(RELINK_BUNDLE_DIR / name).as_posix() for name in REQUIRED_RELINK_FILES]
        )
    )
    patch_paths = tuple(
        (PATCH_BUNDLE_DIR / patch).as_posix() for patch in pins.patches
    )
    if not patched_paths:
        raise BundleError("cannot build SPDX record without patched LibRaw source files")
    required = set(wrapper_paths) | set(patch_paths)
    missing = sorted(required - set(payload))
    if missing:
        raise BundleError(f"cannot build SPDX record; missing source files: {missing}")
    all_paths = patched_paths + wrapper_paths + patch_paths
    file_ids = [_spdx_file_id(path) for path in all_paths]
    if len(file_ids) != len(set(file_ids)):
        raise BundleError("SPDX file identifiers collide for distinct source paths")
    return patched_paths, wrapper_paths, patch_paths


def _spdx_document(
    pins: Pins,
    decision: LicenseDecision,
    payload: Mapping[str, bytes],
    created_at: str,
) -> dict[str, object]:
    route = decision.route
    concluded = route if route != "UNRESOLVED" else "NOASSERTION"
    decision_digest = _sha256(decision.canonical_bytes)
    _parse_canonical_utc_timestamp(created_at, "SPDX source timestamp")
    patched_paths, wrapper_paths, patch_paths = _spdx_source_paths(pins, payload)
    patch_concluded = decision.patch_license or "NOASSERTION"
    files = [
        *(_spdx_file_record(path, payload[path]) for path in patched_paths),
        *(_spdx_file_record(path, payload[path]) for path in wrapper_paths),
        *(
            _spdx_file_record(path, payload[path], patch_concluded)
            for path in patch_paths
        ),
    ]
    inventory = {
        "created_at": created_at,
        "decision_sha256": decision_digest,
        "files": [
            {"path": path, "sha256": _sha256(payload[path])}
            for path in patched_paths + wrapper_paths + patch_paths
        ],
        "route": route,
    }
    inventory_digest = _sha256(_canonical_json(inventory))
    namespace = (
        "https://github.com/thetechgeekko/Spektrafilm-android/spdx/libraw/"
        f"{pins.version}/{pins.archive_sha256}/{pins.patched_tree_sha256}/"
        f"{route}/{inventory_digest}"
    )
    patched_file_ids = [_spdx_file_id(path) for path in patched_paths]
    wrapper_file_ids = [_spdx_file_id(path) for path in wrapper_paths]
    relationships: list[dict[str, str]] = [
        {
            "relatedSpdxElement": package,
            "relationshipType": "DESCRIBES",
            "spdxElementId": "SPDXRef-DOCUMENT",
        }
        for package in (
            "SPDXRef-Package-LibRaw-Pristine",
            "SPDXRef-Package-LibRaw-Patched",
            "SPDXRef-Package-sfraw-wrapper",
        )
    ]
    relationships.extend(
        (
            {
                "relatedSpdxElement": "SPDXRef-Package-LibRaw-Pristine",
                "relationshipType": "GENERATED_FROM",
                "spdxElementId": "SPDXRef-Package-LibRaw-Patched",
            },
            {
                "relatedSpdxElement": "SPDXRef-Package-LibRaw-Patched",
                "relationshipType": "STATIC_LINK",
                "spdxElementId": "SPDXRef-Package-sfraw-wrapper",
            },
        )
    )
    relationships.extend(
        {
            "relatedSpdxElement": "SPDXRef-Package-LibRaw-Patched",
            "relationshipType": "PATCH_APPLIED",
            "spdxElementId": _spdx_file_id(path),
        }
        for path in patch_paths
    )
    return {
        "SPDXID": "SPDXRef-DOCUMENT",
        "creationInfo": {
            "created": created_at,
            "creators": ["Tool: spektrafilm-libraw-bundle-v1"],
        },
        "dataLicense": "CC0-1.0",
        "documentNamespace": namespace,
        "files": files,
        "name": f"Spektrafilm-LibRaw-{pins.version}-compliance",
        "packages": [
            {
                "SPDXID": "SPDXRef-Package-LibRaw-Pristine",
                "checksums": [
                    {
                        "algorithm": "SHA256",
                        "checksumValue": pins.archive_sha256,
                    }
                ],
                "downloadLocation": pins.url,
                "filesAnalyzed": False,
                "licenseConcluded": "NOASSERTION",
                "licenseDeclared": "LGPL-2.1-only OR CDDL-1.0",
                "name": "LibRaw pristine source archive",
                "versionInfo": pins.version,
            },
            {
                "SPDXID": "SPDXRef-Package-LibRaw-Patched",
                "downloadLocation": "NOASSERTION",
                "filesAnalyzed": True,
                "hasFiles": patched_file_ids,
                "licenseConcluded": concluded,
                "licenseDeclared": "LGPL-2.1-only OR CDDL-1.0",
                "licenseInfoFromFiles": ["NOASSERTION"],
                "name": "Spektrafilm patched LibRaw source",
                "packageVerificationCode": {
                    "packageVerificationCodeValue": _package_verification_code(
                        payload, patched_paths
                    )
                },
                "sourceInfo": (
                    f"Official archive plus {len(pins.patches)} ordered, SHA-256-pinned "
                    "Spektrafilm hardening patches"
                ),
                "versionInfo": pins.version,
            },
            {
                "SPDXID": "SPDXRef-Package-sfraw-wrapper",
                "downloadLocation": "NOASSERTION",
                "filesAnalyzed": True,
                "hasFiles": wrapper_file_ids,
                "licenseConcluded": "GPL-3.0-only",
                "licenseDeclared": "GPL-3.0-only",
                "licenseInfoFromFiles": ["NOASSERTION"],
                "name": "Spektrafilm LibRaw JNI wrapper",
                "packageVerificationCode": {
                    "packageVerificationCodeValue": _package_verification_code(
                        payload, wrapper_paths
                    )
                },
                "sourceInfo": (
                    "Exact bundled JNI wrapper and standalone recipient relink source files"
                ),
            },
        ],
        "relationships": relationships,
        "spdxVersion": SPDX_VERSION,
    }


def _modified_paths(repo_root: Path, pins: Pins) -> tuple[str, ...]:
    paths: set[str] = set()
    for patch in pins.patches:
        patch_path = repo_root / PATCH_REPOSITORY_DIR / patch
        try:
            parsed = _parse_patch(patch_path.read_bytes())
        except OSError as error:
            raise BundleError(f"cannot read patch {patch_path}: {error}") from error
        paths.update(relative for relative, _ in parsed)
    ordered = tuple(sorted(paths))
    if len(ordered) != MODIFIED_FILE_COUNT:
        raise BundleError(
            f"ordered patch series modifies {len(ordered)} upstream files; "
            f"expected {MODIFIED_FILE_COUNT}"
        )
    return ordered


def _verify_modification_notices(source_root: Path, modified_paths: Sequence[str]) -> None:
    # Patches are authored on different dates; each file carries its own dated
    # notice (0001-0023 stamp 2026-08-30, 0024/0025 stamp 2026-09-01).
    first = re.compile(
        rb"Modified by Spektrafilm Android contributors, "
        rb"[0-9]{4}-[0-9]{2}-[0-9]{2}; see the"
    )
    second = b"corresponding source distribution's bundled patch manifest."
    for relative in modified_paths:
        path = source_root.joinpath(*PurePosixPath(relative).parts)
        try:
            data = path.read_bytes()
        except OSError as error:
            raise BundleError(f"cannot read modified LibRaw file {relative}: {error}") from error
        if len(first.findall(data)) != 1 or data.count(second) != 1:
            raise BundleError(
                f"modified LibRaw file lacks the exact dated contributor notice: {relative}"
            )


def _modifications_text(
    pins: Pins, route: str, modified_paths: Sequence[str]
) -> bytes:
    lines = [
        "# LibRaw modifications",
        "",
        f"- Upstream version: `{pins.version}`",
        f"- Modification date: `{MODIFICATION_DATE}`",
        "- Contributor: `Spektrafilm Android contributors`",
        f"- Recorded license route: `{route}`",
        f"- Patched-tree SHA-256: `{pins.patched_tree_sha256}`",
        "",
        "The dated notice in each modified upstream file refers to the ordered patch",
        "manifest bundled at `relink/lib/libraw/patches/README.md`.",
        "",
        f"## Modified upstream files ({len(modified_paths)})",
        "",
    ]
    lines.extend(f"- `{path}`" for path in modified_paths)
    lines.extend(("", f"## Ordered patch series ({len(pins.patches)})", ""))
    lines.extend(
        f"{index}. `{patch}` - `{pins.patch_sha256[patch]}`"
        for index, patch in enumerate(pins.patches, start=1)
    )
    lines.extend(("", "This inventory is generated and verified from the pinned resolver.", ""))
    return "\n".join(lines).encode("utf-8")


def _collect_payload(
    repo_root: Path,
    archive: Path,
    source_root: Path,
    pins: Pins,
    decision: LicenseDecision,
    modified_paths: Sequence[str],
) -> dict[str, bytes]:
    route = decision.route
    payload: dict[str, bytes] = {}
    source_prefix = PurePosixPath("source") / f"LibRaw-{pins.version}"
    archive_bytes = archive.read_bytes()
    if (
        len(archive_bytes) != pins.archive_size
        or _sha256(archive_bytes) != pins.archive_sha256
    ):
        raise BundleError("authenticated LibRaw archive changed while building bundle")
    payload[f"source/LibRaw-{pins.version}.tar.gz"] = archive_bytes
    for path in sorted(source_root.rglob("*")):
        if path.is_file():
            relative = PurePosixPath(path.relative_to(source_root).as_posix())
            payload[(source_prefix / relative).as_posix()] = path.read_bytes()

    resolver = repo_root / RESOLVER_REPOSITORY_PATH
    payload[RESOLVER_BUNDLE_PATH] = resolver.read_bytes()
    for patch in pins.patches:
        path = repo_root / PATCH_REPOSITORY_DIR / patch
        payload[(PATCH_BUNDLE_DIR / patch).as_posix()] = path.read_bytes()
    if not (repo_root / PATCH_README_REPOSITORY_PATH).is_file():
        raise BundleError(f"missing LibRaw patch manifest: {PATCH_README_REPOSITORY_PATH}")
    payload[PATCH_README_BUNDLE_PATH] = (
        repo_root / PATCH_README_REPOSITORY_PATH
    ).read_bytes()
    for name in REQUIRED_NATIVE_FILES:
        path = repo_root / NATIVE_REPOSITORY_DIR / name
        if not path.is_file():
            raise BundleError(f"missing native relink source: {path}")
        payload[(NATIVE_BUNDLE_DIR / name).as_posix()] = path.read_bytes()
    for name in REQUIRED_RELINK_FILES:
        path = repo_root / RELINK_REPOSITORY_DIR / name
        if not path.is_file():
            raise BundleError(f"missing standalone recipient relink file: {path}")
        payload[(RELINK_BUNDLE_DIR / name).as_posix()] = path.read_bytes()
    route_path = repo_root / ROUTE_REPOSITORY_PATH
    payload[ROUTE_BUNDLE_PATH] = route_path.read_bytes()
    payload[DECISION_BUNDLE_PATH] = decision.canonical_bytes
    spdx_created_at = _load_spdx_created_at(repo_root / SPDX_CREATED_REPOSITORY_PATH)
    payload[SPDX_CREATED_BUNDLE_PATH] = (
        repo_root / SPDX_CREATED_REPOSITORY_PATH
    ).read_bytes()
    payload[RELINKING_PATH] = _relinking_text(pins.version, route)
    payload[MODIFICATIONS_PATH] = _modifications_text(pins, route, modified_paths)
    for repository_path, bundle_path in (
        (NOTICE_REPOSITORY_PATH, NOTICE_BUNDLE_PATH),
        (PROJECT_LICENSE_REPOSITORY_PATH, PROJECT_LICENSE_BUNDLE_PATH),
        (LICENSING_REPOSITORY_PATH, LICENSING_BUNDLE_PATH),
    ):
        path = repo_root / repository_path
        if not path.is_file():
            raise BundleError(f"missing release notice context: {repository_path}")
        payload[bundle_path] = path.read_bytes()
    payload[SBOM_PATH] = _canonical_json(
        _spdx_document(pins, decision, payload, spdx_created_at)
    )
    return payload


def _verify_patch_files(repo_root: Path, pins: Pins) -> None:
    for patch in pins.patches:
        path = repo_root / PATCH_REPOSITORY_DIR / patch
        if not path.is_file():
            raise BundleError(f"missing pinned LibRaw patch: {path}")
        digest = _file_sha256(path)
        expected = pins.patch_sha256[patch]
        if digest != expected:
            raise BundleError(
                f"LibRaw patch SHA-256 mismatch for {patch}: expected {expected}, got {digest}"
            )


def _repository_bytes(repo_root: Path, relative: Path, description: str) -> bytes:
    path = repo_root / relative
    try:
        return path.read_bytes()
    except OSError as error:
        raise BundleError(f"cannot read {description} {path}: {error}") from error


def _verify_relink_project_contract(repo_root: Path) -> None:
    cmake_bytes = _repository_bytes(
        repo_root,
        RELINK_REPOSITORY_DIR / "CMakeLists.txt",
        "standalone recipient relink project",
    )
    marker_bytes = _repository_bytes(
        repo_root,
        RELINK_REPOSITORY_DIR / "unofficial_relink_marker.cpp",
        "standalone recipient relink marker",
    )
    try:
        cmake = cmake_bytes.decode("utf-8")
        marker = marker_bytes.decode("utf-8")
    except UnicodeDecodeError as error:
        raise BundleError("standalone recipient relink project must be UTF-8") from error
    active_cmake = "\n".join(line.split("#", 1)[0] for line in cmake.splitlines())

    required_cmake_tokens = (
        "LIBRAW_SOURCE_DIR",
        "add_library(raw STATIC",
        "add_library(sfraw SHARED",
        "native_allocation_registry.cpp",
        "raw_decoder.cpp",
        "raw_decoder_jni.cpp",
        "raw_result_publication.cpp",
        "unofficial_relink_marker.cpp",
        "NO_JPEG",
        "USE_ZLIB",
        "LIBRAW_NODLL",
        "OUTPUT_NAME sfraw",
        "-soname,libsfraw.so",
        "max-page-size=16384",
    )
    missing = [token for token in required_cmake_tokens if token not in active_cmake]
    if missing:
        raise BundleError(
            "standalone recipient relink project is missing required ABI/build "
            f"contract tokens: {missing}"
        )
    forbidden_cmake_tokens = (
        "LibRawVendor.cmake",
        "_SFRAW_LIBRAW_PATCHED_TREE_SHA256",
    )
    forbidden = [token for token in forbidden_cmake_tokens if token in active_cmake]
    if forbidden:
        raise BundleError(
            "standalone recipient relink project must not impose the production "
            f"resolver or aggregate gate: {forbidden}"
        )
    if (
        "sfraw_recipient_relink_marker" not in marker
        or "UNOFFICIAL RECIPIENT RELINK" not in marker
    ):
        raise BundleError(
            "standalone recipient relink marker lacks its exported symbol or "
            "unofficial-build notice"
        )


def _manifest(
    pins: Pins, decision: LicenseDecision, payload: Mapping[str, bytes]
) -> dict[str, object]:
    return {
        "files": [
            {"path": path, "sha256": _sha256(data), "size": len(data)}
            for path, data in sorted(payload.items())
        ],
        "libraw": {
            "archive_sha256": pins.archive_sha256,
            "archive_size": pins.archive_size,
            "audited_file_count": AUDITED_FILE_COUNT,
            "commit": pins.commit,
            "patched_tree_sha256": pins.patched_tree_sha256,
            "patches": [
                {"name": patch, "sha256": pins.patch_sha256[patch]}
                for patch in pins.patches
            ],
            "tag_object": pins.tag_object,
            "url": pins.url,
            "version": pins.version,
        },
        "license_decision_sha256": _sha256(decision.canonical_bytes),
        "license_route": decision.route,
        "spdx_created_at": payload[SPDX_CREATED_BUNDLE_PATH]
        .decode("ascii")
        .rstrip("\n"),
        "schema": SCHEMA,
    }


def _zip_info(path: str) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(path, ZIP_TIMESTAMP)
    info.compress_type = zipfile.ZIP_STORED
    info.create_system = 3
    info.create_version = 20
    info.extract_version = 20
    info.flag_bits = 0
    info.internal_attr = 0
    info.extra = b""
    info.comment = b""
    info.external_attr = 0o100644 << 16
    return info


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def _write_bundle(path: Path, manifest: Mapping[str, object], payload: Mapping[str, bytes]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        with zipfile.ZipFile(temporary, mode="w", compression=zipfile.ZIP_STORED) as bundle:
            bundle.writestr(_zip_info(MANIFEST_PATH), _canonical_json(manifest))
            for member, data in sorted(payload.items()):
                bundle.writestr(_zip_info(member), data)
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def _paths_alias(first: Path, second: Path) -> bool:
    if os.path.normcase(str(first.resolve())) == os.path.normcase(str(second.resolve())):
        return True
    try:
        return first.exists() and second.exists() and os.path.samefile(first, second)
    except OSError:
        return False


def _verify_distinct_build_paths(
    archive: Path, output: Path, sbom_output: Path | None
) -> None:
    pairs = [("--archive", archive, "--output", output)]
    if sbom_output is not None:
        pairs.extend(
            (
                ("--archive", archive, "--sbom-output", sbom_output),
                ("--output", output, "--sbom-output", sbom_output),
            )
        )
    for first_name, first, second_name, second in pairs:
        if _paths_alias(first, second):
            raise BundleError(
                f"{first_name} and {second_name} must resolve to distinct files"
            )


def _build(repo_root: Path, archive: Path, output: Path, sbom_output: Path | None) -> None:
    _verify_distinct_build_paths(archive, output, sbom_output)
    pins = _load_pins(repo_root)
    decision = _audit_route_state(repo_root, require_resolved=False)
    _verify_patch_files(repo_root, pins)
    _verify_relink_project_contract(repo_root)
    _verify_archive(archive, pins)
    modified_paths = _modified_paths(repo_root, pins)
    with tempfile.TemporaryDirectory(prefix="sfraw-libraw-compliance-") as temporary:
        source_root = Path(temporary) / f"LibRaw-{pins.version}"
        _extract_archive(archive, source_root, pins)
        for patch in pins.patches:
            _apply_patch(source_root, repo_root / PATCH_REPOSITORY_DIR / patch)
        _verify_patched_source(source_root, pins)
        _verify_modification_notices(source_root, modified_paths)
        payload = _collect_payload(
            repo_root, archive, source_root, pins, decision, modified_paths
        )
        manifest = _manifest(pins, decision, payload)
        _write_bundle(output, manifest, payload)
        if sbom_output is not None:
            _atomic_write(sbom_output, payload[SBOM_PATH])
    _verify_bundle(repo_root, output, require_resolved=False)


def _read_bundle_members(bundle_path: Path) -> tuple[dict[str, bytes], list[zipfile.ZipInfo]]:
    try:
        archive = zipfile.ZipFile(bundle_path, mode="r")
    except (OSError, zipfile.BadZipFile) as error:
        raise BundleError(f"cannot open compliance ZIP {bundle_path}: {error}") from error
    members: dict[str, bytes] = {}
    with archive:
        if archive.comment:
            raise BundleError("compliance ZIP global comment is forbidden")
        infos = archive.infolist()
        if len(infos) > MAX_BUNDLE_FILES:
            raise BundleError("compliance ZIP exceeds the file-count safety limit")
        names = [info.filename for info in infos]
        if len(names) != len(set(names)):
            raise BundleError("compliance ZIP contains duplicate member names")
        total_size = 0
        for info in infos:
            # On Windows zipfile normalizes backslashes in ``filename``. Its
            # ``orig_filename`` preserves the decoded archive entry and must be
            # checked first so a hostile raw name cannot bypass the path policy.
            original_name = getattr(info, "orig_filename", info.filename)
            _validate_safe_relative_path(original_name, "ZIP member path")
            _validate_safe_relative_path(info.filename, "ZIP member path")
            if original_name != info.filename:
                raise BundleError(f"ZIP member name is not canonical: {original_name!r}")
            if info.is_dir():
                raise BundleError(f"directory entries are forbidden in compliance ZIP: {info.filename}")
            if info.flag_bits & 0x1:
                raise BundleError(f"encrypted ZIP member is forbidden: {info.filename}")
            if info.flag_bits != 0:
                raise BundleError(f"unexpected ZIP flags for {info.filename}")
            if info.compress_type != zipfile.ZIP_STORED:
                raise BundleError(f"unexpected ZIP compression for {info.filename}")
            mode = (info.external_attr >> 16) & 0o170000
            if mode != 0o100000:
                raise BundleError(f"non-regular ZIP member is forbidden: {info.filename}")
            if info.external_attr != 0o100644 << 16:
                raise BundleError(
                    f"ZIP member mode is not canonical regular 0644: {info.filename}"
                )
            if info.extra or info.comment:
                raise BundleError(f"ZIP member extra/comment metadata is forbidden: {info.filename}")
            if (
                info.create_system != 3
                or info.create_version != 20
                or info.extract_version != 20
                or info.internal_attr != 0
                or info.reserved != 0
                or info.volume != 0
            ):
                raise BundleError(f"ZIP member version/attribute metadata is not canonical: {info.filename}")
            total_size += info.file_size
            if total_size > MAX_BUNDLE_BYTES:
                raise BundleError("compliance ZIP exceeds the uncompressed-byte safety limit")
            try:
                members[info.filename] = archive.read(info)
            except (OSError, RuntimeError, zipfile.BadZipFile) as error:
                raise BundleError(f"cannot read ZIP member {info.filename}: {error}") from error
    return members, infos


def _expect_dict(value: object, description: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise BundleError(f"{description} must be a JSON object")
    return value


def _expect_repository_member(
    members: Mapping[str, bytes],
    member: str,
    repo_root: Path,
    repository_path: Path,
    description: str,
) -> None:
    expected = _repository_bytes(repo_root, repository_path, description)
    if members.get(member) != expected:
        raise BundleError(f"bundled {description} differs from repository: {member}")


def _verify_bundled_source_tree(
    repo_root: Path,
    members: Mapping[str, bytes],
    pins: Pins,
    modified_paths: Sequence[str],
) -> set[str]:
    archive_member = f"source/LibRaw-{pins.version}.tar.gz"
    archive_bytes = members.get(archive_member)
    if archive_bytes is None:
        raise BundleError(f"bundle is missing authenticated archive: {archive_member}")

    source_prefix = f"source/LibRaw-{pins.version}/"
    bundled_source = {
        member[len(source_prefix) :]: data
        for member, data in members.items()
        if member.startswith(source_prefix)
    }
    with tempfile.TemporaryDirectory(prefix="sfraw-libraw-bundle-verify-") as temporary:
        temporary_root = Path(temporary)
        archive_path = temporary_root / f"LibRaw-{pins.version}.tar.gz"
        archive_path.write_bytes(archive_bytes)
        _verify_archive(archive_path, pins)
        expected_root = temporary_root / f"LibRaw-{pins.version}"
        _extract_archive(archive_path, expected_root, pins)
        for patch in pins.patches:
            _apply_patch(expected_root, repo_root / PATCH_REPOSITORY_DIR / patch)
        _verify_patched_source(expected_root, pins)
        _verify_modification_notices(expected_root, modified_paths)
        expected_source = {
            path.relative_to(expected_root).as_posix(): path.read_bytes()
            for path in expected_root.rglob("*")
            if path.is_file()
        }

    if set(bundled_source) != set(expected_source):
        missing = sorted(set(expected_source) - set(bundled_source))
        extra = sorted(set(bundled_source) - set(expected_source))
        raise BundleError(
            "bundled patched LibRaw tree inventory differs from authenticated "
            f"archive plus pinned patches; missing={missing}, extra={extra}"
        )
    for relative, expected in expected_source.items():
        if bundled_source[relative] != expected:
            raise BundleError(
                "bundled patched LibRaw file differs from authenticated archive plus "
                f"pinned patches: {relative}"
            )
    return {archive_member} | {
        source_prefix + relative for relative in expected_source
    }


def _expected_fixed_payload_names(pins: Pins) -> set[str]:
    names = {
        RESOLVER_BUNDLE_PATH,
        PATCH_README_BUNDLE_PATH,
        ROUTE_BUNDLE_PATH,
        DECISION_BUNDLE_PATH,
        SPDX_CREATED_BUNDLE_PATH,
        RELINKING_PATH,
        MODIFICATIONS_PATH,
        NOTICE_BUNDLE_PATH,
        PROJECT_LICENSE_BUNDLE_PATH,
        LICENSING_BUNDLE_PATH,
        SBOM_PATH,
    }
    names.update((PATCH_BUNDLE_DIR / patch).as_posix() for patch in pins.patches)
    names.update((NATIVE_BUNDLE_DIR / name).as_posix() for name in REQUIRED_NATIVE_FILES)
    names.update((RELINK_BUNDLE_DIR / name).as_posix() for name in REQUIRED_RELINK_FILES)
    return names


def _verify_bundle(repo_root: Path, bundle_path: Path, require_resolved: bool) -> str:
    pins = _load_pins(repo_root)
    _verify_patch_files(repo_root, pins)
    _verify_relink_project_contract(repo_root)
    modified_paths = _modified_paths(repo_root, pins)
    repository_decision = _audit_route_state(repo_root, require_resolved=require_resolved)
    repository_route = repository_decision.route
    members, infos = _read_bundle_members(bundle_path)
    expected_order = [MANIFEST_PATH] + sorted(set(members) - {MANIFEST_PATH})
    if [info.filename for info in infos] != expected_order:
        raise BundleError("compliance ZIP member order is not deterministic")
    for info in infos:
        if info.date_time != ZIP_TIMESTAMP or info.create_system != 3:
            raise BundleError(
                f"compliance ZIP metadata is not deterministic: {info.filename}"
            )
    if MANIFEST_PATH not in members:
        raise BundleError("compliance ZIP is missing manifest.json")
    try:
        manifest_value = json.loads(members[MANIFEST_PATH])
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise BundleError(f"manifest.json is not valid UTF-8 JSON: {error}") from error
    manifest = _expect_dict(manifest_value, "manifest")
    if members[MANIFEST_PATH] != _canonical_json(manifest):
        raise BundleError("manifest.json is not in canonical deterministic form")
    if manifest.get("schema") != SCHEMA:
        raise BundleError(f"unsupported compliance manifest schema: {manifest.get('schema')!r}")
    route = manifest.get("license_route")
    if not isinstance(route, str) or route not in KNOWN_ROUTES:
        raise BundleError("manifest contains an unknown license route")
    if route != repository_route:
        raise BundleError("bundle license route differs from canonical repository marker")
    if manifest.get("license_decision_sha256") != _sha256(
        repository_decision.canonical_bytes
    ):
        raise BundleError(
            "manifest license decision SHA-256 differs from canonical repository decision"
        )
    repository_spdx_created_at = _load_spdx_created_at(
        repo_root / SPDX_CREATED_REPOSITORY_PATH
    )
    if manifest.get("spdx_created_at") != repository_spdx_created_at:
        raise BundleError(
            "manifest SPDX source timestamp differs from canonical repository timestamp"
        )

    libraw = _expect_dict(manifest.get("libraw"), "manifest libraw record")
    expected_libraw_scalars: dict[str, object] = {
        "archive_sha256": pins.archive_sha256,
        "archive_size": pins.archive_size,
        "audited_file_count": AUDITED_FILE_COUNT,
        "commit": pins.commit,
        "patched_tree_sha256": pins.patched_tree_sha256,
        "tag_object": pins.tag_object,
        "url": pins.url,
        "version": pins.version,
    }
    for key, expected in expected_libraw_scalars.items():
        if libraw.get(key) != expected:
            raise BundleError(f"manifest LibRaw {key} does not match pinned resolver")
    expected_patches = [
        {"name": patch, "sha256": pins.patch_sha256[patch]} for patch in pins.patches
    ]
    if libraw.get("patches") != expected_patches:
        raise BundleError("manifest patch order or SHA-256 does not match pinned resolver")

    files_value = manifest.get("files")
    if not isinstance(files_value, list):
        raise BundleError("manifest files must be a JSON array")
    expected_payload_names = set(members) - {MANIFEST_PATH}
    described: set[str] = set()
    for entry_value in files_value:
        entry = _expect_dict(entry_value, "manifest file entry")
        if set(entry) != {"path", "sha256", "size"}:
            raise BundleError("manifest file entry has unexpected fields")
        path = entry.get("path")
        digest = entry.get("sha256")
        size = entry.get("size")
        if not isinstance(path, str):
            raise BundleError("manifest file path must be a string")
        _validate_safe_relative_path(path, "manifest file path")
        if path in described:
            raise BundleError(f"manifest describes a file twice: {path}")
        described.add(path)
        if path not in members:
            raise BundleError(f"manifest describes an absent ZIP member: {path}")
        if not isinstance(digest, str) or digest != _sha256(members[path]):
            raise BundleError(f"payload SHA-256 mismatch: {path}")
        if not isinstance(size, int) or isinstance(size, bool) or size != len(members[path]):
            raise BundleError(f"payload size mismatch: {path}")
    if described != expected_payload_names:
        missing = sorted(expected_payload_names - described)
        extra = sorted(described - expected_payload_names)
        raise BundleError(f"manifest payload inventory mismatch; missing={missing}, extra={extra}")

    route_bytes = members.get(ROUTE_BUNDLE_PATH)
    if route_bytes is None:
        raise BundleError(f"bundle is missing {ROUTE_BUNDLE_PATH}")
    try:
        bundled_route = route_bytes.decode("ascii").splitlines()
    except UnicodeDecodeError as error:
        raise BundleError("bundled license route is not ASCII") from error
    if bundled_route != [route]:
        raise BundleError("bundled license route does not match manifest")

    decision_bytes = members.get(DECISION_BUNDLE_PATH)
    if decision_bytes is None:
        raise BundleError(f"bundle is missing {DECISION_BUNDLE_PATH}")
    bundled_decision = _parse_decision(
        decision_bytes, f"bundled {DECISION_BUNDLE_PATH}"
    )
    if bundled_decision.route != route:
        raise BundleError("bundled license decision route does not match manifest")
    if bundled_decision.canonical_bytes != repository_decision.canonical_bytes:
        raise BundleError("bundled license decision differs from canonical repository decision")
    if require_resolved:
        _require_release_decision(bundled_decision)

    spdx_created_bytes = members.get(SPDX_CREATED_BUNDLE_PATH)
    if spdx_created_bytes is None:
        raise BundleError(f"bundle is missing {SPDX_CREATED_BUNDLE_PATH}")
    try:
        bundled_spdx_created_at = spdx_created_bytes.decode("ascii").rstrip("\n")
    except UnicodeDecodeError as error:
        raise BundleError("bundled SPDX source timestamp is not ASCII") from error
    if spdx_created_bytes != (bundled_spdx_created_at + "\n").encode("ascii"):
        raise BundleError("bundled SPDX source timestamp is not canonical")
    _parse_canonical_utc_timestamp(
        bundled_spdx_created_at, "bundled SPDX source timestamp"
    )
    if bundled_spdx_created_at != repository_spdx_created_at:
        raise BundleError("bundled SPDX source timestamp differs from repository")

    expected_source_names = _verify_bundled_source_tree(
        repo_root, members, pins, modified_paths
    )
    _expect_repository_member(
        members,
        RESOLVER_BUNDLE_PATH,
        repo_root,
        RESOLVER_REPOSITORY_PATH,
        "LibRaw resolver",
    )
    for patch in pins.patches:
        member = (PATCH_BUNDLE_DIR / patch).as_posix()
        if member not in members or _sha256(members[member]) != pins.patch_sha256[patch]:
            raise BundleError(f"bundled patch differs from pinned patch: {patch}")
        _expect_repository_member(
            members,
            member,
            repo_root,
            PATCH_REPOSITORY_DIR / patch,
            f"LibRaw patch {patch}",
        )
    _expect_repository_member(
        members,
        PATCH_README_BUNDLE_PATH,
        repo_root,
        PATCH_README_REPOSITORY_PATH,
        "LibRaw patch manifest",
    )
    for name in REQUIRED_NATIVE_FILES:
        member = (NATIVE_BUNDLE_DIR / name).as_posix()
        _expect_repository_member(
            members,
            member,
            repo_root,
            NATIVE_REPOSITORY_DIR / name,
            f"native relink source {name}",
        )
    for name in REQUIRED_RELINK_FILES:
        member = (RELINK_BUNDLE_DIR / name).as_posix()
        _expect_repository_member(
            members,
            member,
            repo_root,
            RELINK_REPOSITORY_DIR / name,
            f"standalone recipient relink file {name}",
        )
    _expect_repository_member(
        members,
        ROUTE_BUNDLE_PATH,
        repo_root,
        ROUTE_REPOSITORY_PATH,
        "canonical LibRaw license route marker",
    )
    _expect_repository_member(
        members,
        DECISION_BUNDLE_PATH,
        repo_root,
        DECISION_REPOSITORY_PATH,
        "canonical LibRaw license decision",
    )
    _expect_repository_member(
        members,
        SPDX_CREATED_BUNDLE_PATH,
        repo_root,
        SPDX_CREATED_REPOSITORY_PATH,
        "canonical SPDX source timestamp",
    )
    _expect_repository_member(
        members,
        NOTICE_BUNDLE_PATH,
        repo_root,
        NOTICE_REPOSITORY_PATH,
        "release NOTICE.md",
    )
    _expect_repository_member(
        members,
        PROJECT_LICENSE_BUNDLE_PATH,
        repo_root,
        PROJECT_LICENSE_REPOSITORY_PATH,
        "project GPL-3.0-only license",
    )
    _expect_repository_member(
        members,
        LICENSING_BUNDLE_PATH,
        repo_root,
        LICENSING_REPOSITORY_PATH,
        "release docs/LICENSING.md",
    )
    if members.get(RELINKING_PATH) != _relinking_text(pins.version, route):
        raise BundleError(f"{RELINKING_PATH} does not match the deterministic relink guide")
    if members.get(MODIFICATIONS_PATH) != _modifications_text(
        pins, route, modified_paths
    ):
        raise BundleError(
            f"{MODIFICATIONS_PATH} does not match the exact modified-file inventory"
        )

    if SBOM_PATH not in members:
        raise BundleError(f"bundle is missing {SBOM_PATH}")
    try:
        sbom_value = json.loads(members[SBOM_PATH])
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise BundleError(f"{SBOM_PATH} is not valid UTF-8 JSON: {error}") from error
    expected_sbom = _spdx_document(
        pins, repository_decision, members, repository_spdx_created_at
    )
    if sbom_value != expected_sbom or members[SBOM_PATH] != _canonical_json(expected_sbom):
        raise BundleError(f"{SBOM_PATH} does not match the pinned deterministic SPDX record")

    expected_names = (
        {MANIFEST_PATH}
        | expected_source_names
        | _expected_fixed_payload_names(pins)
    )
    if set(members) != expected_names:
        missing = sorted(expected_names - set(members))
        extra = sorted(set(members) - expected_names)
        raise BundleError(
            f"compliance ZIP has an unexpected payload inventory; missing={missing}, "
            f"extra={extra}"
        )
    with tempfile.TemporaryDirectory(prefix="sfraw-libraw-zip-canonical-") as temporary:
        canonical_bundle = Path(temporary) / "canonical.zip"
        canonical_payload = {
            path: data for path, data in members.items() if path != MANIFEST_PATH
        }
        _write_bundle(canonical_bundle, manifest, canonical_payload)
        if canonical_bundle.read_bytes() != bundle_path.read_bytes():
            raise BundleError(
                "compliance ZIP bytes do not match the canonical ZIP_STORED encoding"
            )
    return route


def _resolved_path(value: str) -> Path:
    return Path(value).resolve()


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Build or verify a deterministic LibRaw source/relink compliance ZIP. "
            "UNRESOLVED and CDDL-1.0 are known record values, but release verification "
            "accepts only LGPL-2.1-only."
        )
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    build = subparsers.add_parser(
        "build",
        help="authenticate the explicit upstream archive and build a deterministic ZIP",
    )
    build.add_argument("--repo-root", required=True, type=_resolved_path)
    build.add_argument(
        "--archive",
        required=True,
        type=_resolved_path,
        help="explicit official LibRaw tar.gz; no _deps discovery is performed",
    )
    build.add_argument("--output", required=True, type=_resolved_path)
    build.add_argument(
        "--sbom-output",
        type=_resolved_path,
        help=f"optional standalone byte-identical copy of ZIP member {SBOM_PATH}",
    )
    verify = subparsers.add_parser("verify", help="fail closed on any bundle drift")
    verify.add_argument("--repo-root", required=True, type=_resolved_path)
    verify.add_argument("--bundle", required=True, type=_resolved_path)
    verify.add_argument(
        "--require-resolved",
        action="store_true",
        help="release gate: accept only LGPL-2.1-only; reject UNRESOLVED and CDDL-1.0",
    )
    audit = subparsers.add_parser(
        "audit-route",
        help="validate the decision record and every cross-file route marker without an archive",
    )
    audit.add_argument("--repo-root", required=True, type=_resolved_path)
    audit.add_argument(
        "--require-resolved",
        action="store_true",
        help="release gate: require synchronized LGPL approval and patch authorization",
    )
    return parser


def main(arguments: Sequence[str] | None = None) -> int:
    parser = _parser()
    options = parser.parse_args(arguments)
    try:
        if options.command == "build":
            _build(options.repo_root, options.archive, options.output, options.sbom_output)
            print(f"built deterministic LibRaw compliance bundle: {options.output}")
        elif options.command == "verify":
            route = _verify_bundle(
                options.repo_root, options.bundle, options.require_resolved
            )
            print(f"verified LibRaw compliance bundle ({route}): {options.bundle}")
        else:
            decision = _audit_route_state(options.repo_root, options.require_resolved)
            print(f"audited LibRaw license route ({decision.route}): {options.repo_root}")
    except BundleError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Generate Spektrafilm's deterministic SPDX 2.3 application SBOM.

SPDX-License-Identifier: GPL-3.0-only

The release workflow treats this tool as a fail-closed provenance boundary.  It
accepts only the repository's complete, explicit Gradle lockfile inventory and
binds the resulting document to those files, the resolved runtime report, the
Gradle wrapper, the exact source commit, and the canonical LibRaw SPDX document.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import hashlib
import ipaddress
import json
import os
import pathlib
import re
import sys
import tempfile
import urllib.parse
from collections.abc import Iterable, Sequence

APP_SPDX_ID = "SPDXRef-Package-Spektrafilm-Android"
LIBRAW_DOCUMENT_ID = "DocumentRef-LibRaw"
LIBRAW_WRAPPER_ID = "SPDXRef-Package-sfraw-wrapper"
EXPECTED_LOCKFILES = (
    "settings-gradle.lockfile",
    "app/gradle.lockfile",
    "engine/spektra-core/gradle.lockfile",
    "lib/libraw/gradle.lockfile",
    "lib/pngwriter/gradle.lockfile",
    "lib/tiffwriter/gradle.lockfile",
)
REQUIRED_REPOSITORY_INPUTS = (
    "gradle/wrapper/gradle-wrapper.properties",
    "gradle/wrapper/gradle-wrapper.jar",
)
OPTIONAL_REPOSITORY_INPUTS = ("gradle/libs.versions.toml",)
IGNORED_SCAN_DIRECTORIES = frozenset({".git", ".gradle", "build", "__pycache__"})

COORDINATE_RE = re.compile(
    r"(?P<group>[A-Za-z0-9_.-]+):"
    r"(?P<artifact>[A-Za-z0-9_.-]+):"
    r"(?P<version>[A-Za-z0-9][A-Za-z0-9._+!~-]*)"
)
CONFIGURATION_RE = re.compile(r"[A-Za-z0-9_.-]+")
VERSION_RE = re.compile(
    r"v(?:0|[1-9][0-9]*)\."
    r"(?:0|[1-9][0-9]*)\."
    r"(?:0|[1-9][0-9]*)"
)
SOURCE_SHA_RE = re.compile(r"[0-9a-f]{40}")
SHA1_RE = re.compile(r"[0-9a-f]{40}")
SPDX_ELEMENT_ID_RE = re.compile(r"SPDXRef-[A-Za-z0-9.-]+")
RFC3986_REG_NAME_RE = re.compile(
    r"(?:[A-Za-z0-9._~!$&'()*+,;=-]|%[0-9A-Fa-f]{2})+"
)
RFC3986_IPVFUTURE_RE = re.compile(
    r"[vV][0-9A-Fa-f]+\.[A-Za-z0-9._~!$&'()*+,;=:-]+"
)
RFC3986_PATH_RE = re.compile(
    r"(?:[A-Za-z0-9._~!$&'()*+,;=:@/-]|%[0-9A-Fa-f]{2})*"
)
SPDX_23_CHECKSUM_ALGORITHMS = frozenset(
    {
        "ADLER32",
        "BLAKE2b-256",
        "BLAKE2b-384",
        "BLAKE2b-512",
        "BLAKE3",
        "MD2",
        "MD4",
        "MD5",
        "MD6",
        "SHA1",
        "SHA224",
        "SHA256",
        "SHA3-256",
        "SHA3-384",
        "SHA3-512",
        "SHA384",
        "SHA512",
    }
)


class SpdxGenerationError(ValueError):
    """A release input violated the deterministic SPDX contract."""


@dataclasses.dataclass(frozen=True, order=True)
class MavenCoordinate:
    group: str
    artifact: str
    version: str

    @property
    def gav(self) -> str:
        return f"{self.group}:{self.artifact}:{self.version}"

    @property
    def ga(self) -> str:
        return f"{self.group}:{self.artifact}"


@dataclasses.dataclass(frozen=True)
class InputFile:
    display_name: str
    path: pathlib.Path
    sha256: str


@dataclasses.dataclass(frozen=True)
class LibRawDocument:
    namespace: str
    sha256: str


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _require_regular_file(path: pathlib.Path, label: str, *, nonempty: bool = True) -> None:
    if path.is_symlink() or not path.is_file():
        raise SpdxGenerationError(f"{label} must be a regular, non-symlink file: {path}")
    if nonempty and path.stat().st_size == 0:
        raise SpdxGenerationError(f"{label} must not be empty: {path}")


def _scan_lockfile_paths(repo_root: pathlib.Path) -> set[str]:
    actual: set[str] = set()

    def fail_on_scan_error(error: OSError) -> None:
        raise SpdxGenerationError(f"cannot inspect lockfile inventory: {error}") from error

    for directory, child_directories, file_names in os.walk(
        repo_root, onerror=fail_on_scan_error
    ):
        child_directories[:] = sorted(
            name for name in child_directories if name not in IGNORED_SCAN_DIRECTORIES
        )
        base = pathlib.Path(directory)
        for name in sorted(file_names):
            if name not in {"gradle.lockfile", "settings-gradle.lockfile"}:
                continue
            relative = (base / name).relative_to(repo_root).as_posix()
            actual.add(relative)
    return actual


def _validate_lockfile_inventory(repo_root: pathlib.Path) -> list[pathlib.Path]:
    expected = set(EXPECTED_LOCKFILES)
    actual = _scan_lockfile_paths(repo_root)
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        details: list[str] = []
        if missing:
            details.append("missing=" + ",".join(missing))
        if extra:
            details.append("extra=" + ",".join(extra))
        raise SpdxGenerationError("lockfile inventory mismatch: " + "; ".join(details))

    paths: list[pathlib.Path] = []
    for relative in EXPECTED_LOCKFILES:
        path = repo_root / pathlib.PurePosixPath(relative)
        _require_regular_file(path, f"lockfile {relative}")
        paths.append(path)
    return paths


def _parse_configurations(text: str, path: str, line_number: int) -> tuple[str, ...]:
    if not text:
        raise SpdxGenerationError(f"empty Gradle configuration list at {path}:{line_number}")
    configurations = text.split(",")
    if any(not CONFIGURATION_RE.fullmatch(item) for item in configurations):
        raise SpdxGenerationError(
            f"malformed Gradle configuration list at {path}:{line_number}: {text!r}"
        )
    if len(configurations) != len(set(configurations)):
        raise SpdxGenerationError(
            f"duplicate Gradle configuration at {path}:{line_number}: {text!r}"
        )
    return tuple(configurations)


def _parse_lockfiles(
    repo_root: pathlib.Path,
    lockfiles: Iterable[pathlib.Path],
) -> dict[MavenCoordinate, dict[str, set[str]]]:
    scopes: dict[MavenCoordinate, dict[str, set[str]]] = {}
    # Different versions are legitimate in isolated Gradle configurations.  A
    # single module/configuration scope resolving two versions is contradictory.
    scoped_versions: dict[tuple[str, str, str, str], str] = {}
    empty_configurations: dict[str, set[str]] = {}
    dependency_configurations: dict[str, set[str]] = {}

    for path in lockfiles:
        relative = path.relative_to(repo_root).as_posix()
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError as error:
            raise SpdxGenerationError(f"lockfile is not UTF-8: {relative}") from error

        saw_record = False
        for line_number, raw_line in enumerate(lines, start=1):
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            saw_record = True
            if line.count("=") != 1:
                raise SpdxGenerationError(
                    f"malformed Gradle lock entry at {relative}:{line_number}: {line!r}"
                )
            coordinate_text, configurations_text = line.split("=", 1)
            if coordinate_text == "empty":
                # Gradle writes the valid sentinel ``empty=`` when there are no
                # dependency-free configurations to record.  An empty RHS is
                # permitted only for this sentinel.
                configurations = (
                    _parse_configurations(configurations_text, relative, line_number)
                    if configurations_text
                    else ()
                )
                overlap = set(configurations) & dependency_configurations.setdefault(
                    relative, set()
                )
                if overlap:
                    raise SpdxGenerationError(
                        f"Gradle configurations are both empty and dependency-bearing "
                        f"in {relative}: {','.join(sorted(overlap))}"
                    )
                empty_configurations.setdefault(relative, set()).update(configurations)
                continue

            configurations = _parse_configurations(
                configurations_text, relative, line_number
            )
            overlap = set(configurations) & empty_configurations.setdefault(relative, set())
            if overlap:
                raise SpdxGenerationError(
                    f"Gradle configurations are both empty and dependency-bearing "
                    f"in {relative}: {','.join(sorted(overlap))}"
                )
            dependency_configurations.setdefault(relative, set()).update(configurations)

            match = COORDINATE_RE.fullmatch(coordinate_text)
            if match is None:
                raise SpdxGenerationError(
                    f"malformed Maven coordinate at {relative}:{line_number}: "
                    f"{coordinate_text!r}"
                )
            coordinate = MavenCoordinate(**match.groupdict())
            coordinate_scopes = scopes.setdefault(coordinate, {})
            coordinate_scopes.setdefault(relative, set()).update(configurations)

            for configuration in configurations:
                scope_key = (
                    relative,
                    configuration,
                    coordinate.group,
                    coordinate.artifact,
                )
                existing_version = scoped_versions.get(scope_key)
                if existing_version is not None and existing_version != coordinate.version:
                    raise SpdxGenerationError(
                        f"version conflict for {coordinate.ga} in {relative} "
                        f"configuration {configuration}: {existing_version} vs "
                        f"{coordinate.version}"
                    )
                scoped_versions[scope_key] = coordinate.version

        if not saw_record:
            raise SpdxGenerationError(f"lockfile contains no lock records: {relative}")

    return scopes


def _is_valid_rfc3986_authority(netloc: str, parsed_port: int | None) -> bool:
    if netloc.startswith("["):
        closing_bracket = netloc.find("]")
        if closing_bracket < 0:
            return False
        host_text = netloc[1:closing_bracket]
        suffix = netloc[closing_bracket + 1 :]
        # RFC 3986 has no IPv6 zone/scope-ID URI syntax.  In particular, do
        # not inherit ipaddress.IPv6Address's intentionally permissive scope
        # parser for a canonical document namespace.
        if "%" in host_text:
            return False
        if suffix:
            if re.fullmatch(r":[0-9]+", suffix) is None:
                return False
            if parsed_port != int(suffix[1:]):
                return False
        elif parsed_port is not None:
            return False
        try:
            ipaddress.IPv6Address(host_text)
            return True
        except ipaddress.AddressValueError:
            return RFC3986_IPVFUTURE_RE.fullmatch(host_text) is not None

    if ":" in netloc:
        host_text, port_text = netloc.rsplit(":", 1)
        if not port_text.isdecimal() or parsed_port != int(port_text):
            return False
    else:
        host_text = netloc
        if parsed_port is not None:
            return False
    return RFC3986_REG_NAME_RE.fullmatch(host_text) is not None


def _validate_libraw_namespace(value: object) -> str:
    message = "LibRaw documentNamespace must be a valid HTTPS URI"
    if not isinstance(value, str) or not value or not value.isascii():
        raise SpdxGenerationError(message)
    if (
        any(character.isspace() or ord(character) < 0x20 or ord(character) == 0x7F for character in value)
        or "\\" in value
        or re.search(r"%(?![0-9A-Fa-f]{2})", value)
    ):
        raise SpdxGenerationError(message)
    try:
        parsed = urllib.parse.urlsplit(value)
        hostname = parsed.hostname
        # Accessing ``port`` is deliberate: urlsplit otherwise accepts a
        # non-numeric or out-of-range authority port and raises only lazily.
        port = parsed.port
        username = parsed.username
        password = parsed.password
    except ValueError as error:
        raise SpdxGenerationError(message) from error
    authority_is_valid = bool(hostname) and _is_valid_rfc3986_authority(
        parsed.netloc, port
    )
    path_segments = [urllib.parse.unquote(segment) for segment in parsed.path.split("/")]
    if (
        parsed.scheme != "https"
        or not parsed.netloc
        or not authority_is_valid
        or username is not None
        or password is not None
        or not parsed.path.startswith("/")
        or RFC3986_PATH_RE.fullmatch(parsed.path) is None
        or any(segment in {".", ".."} for segment in path_segments)
        or parsed.query
        or parsed.fragment
        or "/spdx/libraw/" not in parsed.path
        or parsed.geturl() != value
    ):
        raise SpdxGenerationError(
            "LibRaw documentNamespace must be an uncredentialed HTTPS LibRaw SPDX URI"
        )
    return value


def _required_nonempty_string(container: dict[str, object], key: str, label: str) -> str:
    value = container.get(key)
    if not isinstance(value, str) or not value.strip():
        raise SpdxGenerationError(f"{label} must contain a non-empty {key}")
    return value


def _validate_libraw_files(
    document: dict[str, object], element_ids: set[str]
) -> dict[str, tuple[str, str | None]]:
    files = document.get("files", [])
    if not isinstance(files, list):
        raise SpdxGenerationError("LibRaw SPDX document files must be an array")
    document_files: dict[str, tuple[str, str | None]] = {}
    for file_entry in files:
        if not isinstance(file_entry, dict):
            raise SpdxGenerationError("LibRaw SPDX file entry must be an object")
        identifier = file_entry.get("SPDXID")
        if not isinstance(identifier, str) or SPDX_ELEMENT_ID_RE.fullmatch(identifier) is None:
            raise SpdxGenerationError("LibRaw SPDX file contains an invalid SPDXID")
        if identifier in element_ids:
            raise SpdxGenerationError(
                f"LibRaw SPDX document-wide SPDXID collision: {identifier}"
            )
        element_ids.add(identifier)
        file_name = _required_nonempty_string(file_entry, "fileName", "LibRaw SPDX file")
        checksums = file_entry.get("checksums")
        if not isinstance(checksums, list) or not checksums:
            raise SpdxGenerationError(
                "LibRaw SPDX file checksums must be a non-empty array"
            )
        checksum_algorithms: set[str] = set()
        sha1_value: str | None = None
        for checksum in checksums:
            if not isinstance(checksum, dict):
                raise SpdxGenerationError("LibRaw SPDX file checksum must be an object")
            if set(checksum) != {"algorithm", "checksumValue"}:
                raise SpdxGenerationError(
                    "LibRaw SPDX file checksum must contain exactly algorithm and checksumValue"
                )
            algorithm = checksum["algorithm"]
            checksum_value = checksum["checksumValue"]
            if (
                not isinstance(algorithm, str)
                or algorithm not in SPDX_23_CHECKSUM_ALGORITHMS
            ):
                raise SpdxGenerationError(
                    f"LibRaw SPDX file has an unsupported checksum algorithm: {algorithm!r}"
                )
            if algorithm in checksum_algorithms:
                raise SpdxGenerationError(
                    f"LibRaw SPDX file has duplicate {algorithm} checksums: {identifier}"
                )
            checksum_algorithms.add(algorithm)
            if not isinstance(checksum_value, str) or re.fullmatch(
                r"[0-9a-f]+", checksum_value
            ) is None:
                raise SpdxGenerationError(
                    f"LibRaw SPDX file has an invalid {algorithm} checksum: {identifier}"
                )
            if algorithm == "SHA1":
                if SHA1_RE.fullmatch(checksum_value) is None:
                    raise SpdxGenerationError(
                        f"LibRaw SPDX file has an invalid SHA1 checksum: {identifier}"
                    )
                sha1_value = checksum_value
        document_files[identifier] = (file_name, sha1_value)
    return document_files


def _validate_libraw_wrapper(
    package: dict[str, object], document_files: dict[str, tuple[str, str | None]]
) -> None:
    label = "LibRaw wrapper package"
    for key in (
        "name",
        "downloadLocation",
        "licenseConcluded",
        "licenseDeclared",
    ):
        _required_nonempty_string(package, key, label)
    if "copyrightText" in package:
        _required_nonempty_string(package, "copyrightText", label)

    files_analyzed = package.get("filesAnalyzed")
    if not isinstance(files_analyzed, bool):
        raise SpdxGenerationError(
            f"{label} must declare filesAnalyzed explicitly as true or false"
        )
    verification_code = package.get("packageVerificationCode")
    has_files = package.get("hasFiles")
    if not files_analyzed:
        if "packageVerificationCode" in package or has_files not in (None, []):
            raise SpdxGenerationError(
                f"{label} with filesAnalyzed=false must not contain analyzed files "
                "or a packageVerificationCode"
            )
        return

    if not isinstance(verification_code, dict):
        raise SpdxGenerationError(
            f"{label} with filesAnalyzed=true requires a packageVerificationCode"
        )
    verification_value = verification_code.get("packageVerificationCodeValue")
    if not isinstance(verification_value, str) or SHA1_RE.fullmatch(verification_value) is None:
        raise SpdxGenerationError(
            f"{label} packageVerificationCodeValue must be 40 lowercase hex characters"
        )
    if not isinstance(has_files, list) or not has_files:
        raise SpdxGenerationError(
            f"{label} with filesAnalyzed=true must reference at least one file"
        )
    if any(
        not isinstance(identifier, str)
        or SPDX_ELEMENT_ID_RE.fullmatch(identifier) is None
        for identifier in has_files
    ):
        raise SpdxGenerationError(f"{label} has an invalid hasFiles SPDXID")
    if len(has_files) != len(set(has_files)):
        raise SpdxGenerationError(f"{label} has duplicate hasFiles SPDXIDs")

    missing_files = sorted(set(has_files) - set(document_files))
    if missing_files:
        raise SpdxGenerationError(
            f"{label} references missing file SPDXIDs: {','.join(missing_files)}"
        )
    referenced_files = [document_files[identifier] for identifier in has_files]
    if any(sha1_value is None for _, sha1_value in referenced_files):
        raise SpdxGenerationError(f"{label} referenced files must each contain a SHA1 checksum")
    referenced_names = [file_name for file_name, _ in referenced_files]
    if len(referenced_names) != len(set(referenced_names)):
        raise SpdxGenerationError(f"{label} referenced fileName values must be unique")

    excluded_files = verification_code.get("packageVerificationCodeExcludedFiles", [])
    if (
        not isinstance(excluded_files, list)
        or any(
            not isinstance(file_name, str) or not file_name.strip()
            for file_name in excluded_files
        )
        or len(excluded_files) != len(set(excluded_files))
    ):
        raise SpdxGenerationError(
            f"{label} packageVerificationCodeExcludedFiles must be a unique string array"
        )
    missing_exclusions = sorted(set(excluded_files) - set(referenced_names))
    if missing_exclusions:
        raise SpdxGenerationError(
            f"{label} excludes files it does not reference: {','.join(missing_exclusions)}"
        )
    included_sha1_values = [
        sha1_value
        for file_name, sha1_value in referenced_files
        if file_name not in excluded_files and sha1_value is not None
    ]
    expected_verification = hashlib.sha1(
        "".join(sorted(included_sha1_values)).encode("ascii")
    ).hexdigest()
    if verification_value != expected_verification:
        raise SpdxGenerationError(
            f"{label} packageVerificationCode does not match its referenced file SHA1 set"
        )


def _load_libraw_document(path: pathlib.Path) -> LibRawDocument:
    _require_regular_file(path, "LibRaw SPDX document")
    raw = path.read_bytes()
    try:
        document = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SpdxGenerationError("LibRaw SPDX document is not valid UTF-8 JSON") from error
    if not isinstance(document, dict):
        raise SpdxGenerationError("LibRaw SPDX document root must be an object")
    if document.get("spdxVersion") != "SPDX-2.3":
        raise SpdxGenerationError("LibRaw SPDX document must declare SPDX-2.3")
    if document.get("SPDXID") != "SPDXRef-DOCUMENT":
        raise SpdxGenerationError("LibRaw SPDX document has an invalid document SPDXID")
    if document.get("dataLicense") != "CC0-1.0":
        raise SpdxGenerationError("LibRaw SPDX document must declare CC0-1.0 dataLicense")
    _required_nonempty_string(document, "name", "LibRaw SPDX document")
    creation_info = document.get("creationInfo")
    if not isinstance(creation_info, dict):
        raise SpdxGenerationError("LibRaw SPDX document creationInfo must be an object")
    _required_nonempty_string(creation_info, "created", "LibRaw SPDX creationInfo")
    creators = creation_info.get("creators")
    if (
        not isinstance(creators, list)
        or not creators
        or any(not isinstance(creator, str) or not creator.strip() for creator in creators)
    ):
        raise SpdxGenerationError(
            "LibRaw SPDX creationInfo creators must be a non-empty string array"
        )

    namespace = _validate_libraw_namespace(document.get("documentNamespace"))

    packages = document.get("packages")
    if not isinstance(packages, list):
        raise SpdxGenerationError("LibRaw SPDX document packages must be an array")
    snippets = document.get("snippets", [])
    if not isinstance(snippets, list) or snippets:
        raise SpdxGenerationError(
            "LibRaw SPDX document snippets must be an empty array for this release contract"
        )
    element_ids = {"SPDXRef-DOCUMENT"}
    wrapper_packages: list[dict[str, object]] = []
    for package in packages:
        if not isinstance(package, dict):
            raise SpdxGenerationError("LibRaw SPDX document contains a non-object package")
        identifier = package.get("SPDXID")
        if not isinstance(identifier, str) or SPDX_ELEMENT_ID_RE.fullmatch(identifier) is None:
            raise SpdxGenerationError("LibRaw SPDX document contains an invalid package SPDXID")
        if identifier in element_ids:
            raise SpdxGenerationError(
                f"LibRaw SPDX document-wide SPDXID collision: {identifier}"
            )
        element_ids.add(identifier)
        if identifier == LIBRAW_WRAPPER_ID:
            wrapper_packages.append(package)
    if len(wrapper_packages) != 1:
        raise SpdxGenerationError(
            f"LibRaw SPDX document must contain exactly one {LIBRAW_WRAPPER_ID} package"
        )
    document_files = _validate_libraw_files(document, element_ids)
    _validate_libraw_wrapper(wrapper_packages[0], document_files)
    return LibRawDocument(namespace=namespace, sha256=_sha256_bytes(raw))


def _collect_input_files(
    repo_root: pathlib.Path,
    lockfiles: Iterable[pathlib.Path],
    runtime_report: pathlib.Path,
) -> list[InputFile]:
    _require_regular_file(runtime_report, "release runtime report")
    records: list[tuple[str, pathlib.Path]] = [
        (f"./{path.relative_to(repo_root).as_posix()}", path) for path in lockfiles
    ]
    for relative in REQUIRED_REPOSITORY_INPUTS:
        path = repo_root / pathlib.PurePosixPath(relative)
        _require_regular_file(path, f"release provenance input {relative}")
        records.append((f"./{relative}", path))
    for relative in OPTIONAL_REPOSITORY_INPUTS:
        path = repo_root / pathlib.PurePosixPath(relative)
        if path.exists():
            _require_regular_file(
                path, f"optional release provenance input {relative}", nonempty=False
            )
            records.append((f"./{relative}", path))
    records.append(("./release-runtime-classpath.txt", runtime_report))

    display_names = [name for name, _ in records]
    if len(display_names) != len(set(display_names)):
        raise SpdxGenerationError("release provenance input names are not unique")
    return sorted(
        (
            InputFile(display_name=name, path=path, sha256=_sha256_file(path))
            for name, path in records
        ),
        key=lambda item: item.display_name,
    )


def _created_at(source_date_epoch: str) -> str:
    if not re.fullmatch(r"(?:0|[1-9][0-9]*)", source_date_epoch):
        raise SpdxGenerationError("source-date-epoch must be a non-negative decimal integer")
    try:
        timestamp = dt.datetime.fromtimestamp(int(source_date_epoch), tz=dt.timezone.utc)
    except (OverflowError, OSError, ValueError) as error:
        raise SpdxGenerationError("source-date-epoch is outside the supported UTC range") from error
    return timestamp.strftime("%Y-%m-%dT%H:%M:%SZ")


def _validate_identity(version: str, source_sha: str) -> tuple[str, str]:
    if VERSION_RE.fullmatch(version) is None:
        raise SpdxGenerationError(
            "version must be a stable vMAJOR.MINOR.PATCH tag without leading zeroes"
        )
    if SOURCE_SHA_RE.fullmatch(source_sha) is None:
        raise SpdxGenerationError("source-sha must be exactly 40 lowercase hex characters")
    return version, source_sha


def _maven_spdx_id(coordinate: MavenCoordinate) -> str:
    return "SPDXRef-Maven-" + _sha256_bytes(coordinate.gav.encode("utf-8"))


def _file_spdx_id(display_name: str) -> str:
    return "SPDXRef-File-" + _sha256_bytes(display_name.encode("utf-8"))


def _purl_segment(value: str) -> str:
    return urllib.parse.quote(value, safe="._-~")


def _scope_comment(scope_map: dict[str, set[str]]) -> str:
    entries = [
        f"{lockfile}[{','.join(sorted(configurations))}]"
        for lockfile, configurations in sorted(scope_map.items())
    ]
    return "Gradle lock scopes: " + "; ".join(entries)


def _assert_unique_spdx_ids(packages: list[dict[str, object]], files: list[dict[str, object]]) -> None:
    owners: dict[str, str] = {}
    for kind, elements in (("package", packages), ("file", files)):
        for element in elements:
            identifier = element.get("SPDXID")
            if not isinstance(identifier, str):
                raise SpdxGenerationError(f"generated {kind} is missing an SPDXID")
            existing = owners.get(identifier)
            if existing is not None:
                raise SpdxGenerationError(
                    f"generated SPDXID collision: {identifier} ({existing} and {kind})"
                )
            owners[identifier] = kind


def _build_document(
    *,
    version: str,
    source_sha: str,
    created: str,
    dependencies: dict[MavenCoordinate, dict[str, set[str]]],
    inputs: list[InputFile],
    libraw: LibRawDocument,
) -> dict[str, object]:
    input_fingerprint = {
        "schema": 1,
        "version": version,
        "sourceSha": source_sha,
        "created": created,
        "libraw": {"namespace": libraw.namespace, "sha256": libraw.sha256},
        "inputs": [[item.display_name, item.sha256] for item in inputs],
        "dependencies": [
            [
                coordinate.gav,
                [
                    [lockfile, sorted(configurations)]
                    for lockfile, configurations in sorted(scope_map.items())
                ],
            ]
            for coordinate, scope_map in sorted(dependencies.items())
        ],
    }
    fingerprint_bytes = json.dumps(
        input_fingerprint,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    fingerprint = _sha256_bytes(fingerprint_bytes)
    quoted_version = urllib.parse.quote(version, safe="._-")
    namespace = (
        "https://github.com/thetechgeekko/Spektrafilm-android/spdx/app/"
        f"{quoted_version}/{source_sha}/{fingerprint}"
    )

    files: list[dict[str, object]] = [
        {
            "SPDXID": _file_spdx_id(item.display_name),
            "fileName": item.display_name,
            "checksums": [
                {"algorithm": "SHA256", "checksumValue": item.sha256}
            ],
            "licenseConcluded": "NOASSERTION",
            "licenseInfoInFiles": ["NOASSERTION"],
            "copyrightText": "NOASSERTION",
            "comment": "Release provenance input hashed before signing.",
        }
        for item in inputs
    ]

    application: dict[str, object] = {
        "SPDXID": APP_SPDX_ID,
        "name": "Spektrafilm Android",
        "versionInfo": version.removeprefix("v"),
        "downloadLocation": (
            "https://github.com/thetechgeekko/Spektrafilm-android/tree/" + source_sha
        ),
        "homepage": "https://github.com/thetechgeekko/Spektrafilm-android",
        "filesAnalyzed": False,
        "licenseConcluded": "GPL-3.0-only",
        "licenseDeclared": "GPL-3.0-only",
        "copyrightText": "NOASSERTION",
        "primaryPackagePurpose": "APPLICATION",
        "comment": f"Release tag {version}; source Git object {source_sha}.",
    }
    maven_packages: list[dict[str, object]] = []
    for coordinate, scope_map in sorted(dependencies.items()):
        purl = (
            f"pkg:maven/{_purl_segment(coordinate.group)}/"
            f"{_purl_segment(coordinate.artifact)}@{_purl_segment(coordinate.version)}"
        )
        maven_packages.append(
            {
                "SPDXID": _maven_spdx_id(coordinate),
                "name": coordinate.ga,
                "versionInfo": coordinate.version,
                "downloadLocation": "NOASSERTION",
                "filesAnalyzed": False,
                "licenseConcluded": "NOASSERTION",
                "licenseDeclared": "NOASSERTION",
                "copyrightText": "NOASSERTION",
                "supplier": "NOASSERTION",
                "primaryPackagePurpose": "LIBRARY",
                "externalRefs": [
                    {
                        "referenceCategory": "PACKAGE-MANAGER",
                        "referenceType": "purl",
                        "referenceLocator": purl,
                    }
                ],
                "comment": _scope_comment(scope_map),
            }
        )
    packages = [application, *maven_packages]
    _assert_unique_spdx_ids(packages, files)

    relationships: list[dict[str, str]] = [
        {
            "spdxElementId": "SPDXRef-DOCUMENT",
            "relationshipType": "DESCRIBES",
            "relatedSpdxElement": APP_SPDX_ID,
        },
        {
            "spdxElementId": APP_SPDX_ID,
            "relationshipType": "DEPENDS_ON",
            "relatedSpdxElement": (
                f"{LIBRAW_DOCUMENT_ID}:{LIBRAW_WRAPPER_ID}"
            ),
        },
    ]
    relationships.extend(
        {
            "spdxElementId": APP_SPDX_ID,
            "relationshipType": "DEPENDS_ON",
            "relatedSpdxElement": package["SPDXID"],
        }
        for package in maven_packages
    )
    relationships.extend(
        {
            "spdxElementId": APP_SPDX_ID,
            "relationshipType": "GENERATED_FROM",
            "relatedSpdxElement": file_entry["SPDXID"],
        }
        for file_entry in files
    )
    relationships.sort(
        key=lambda relationship: (
            relationship["spdxElementId"],
            relationship["relationshipType"],
            relationship["relatedSpdxElement"],
        )
    )

    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"Spektrafilm Android {version} application SBOM",
        "documentNamespace": namespace,
        "creationInfo": {
            "created": created,
            "creators": ["Tool: spektrafilm-app-spdx-v1"],
        },
        "comment": f"Exact release source Git object: {source_sha}",
        "externalDocumentRefs": [
            {
                "externalDocumentId": LIBRAW_DOCUMENT_ID,
                "spdxDocument": libraw.namespace,
                "checksum": {
                    "algorithm": "SHA256",
                    "checksumValue": libraw.sha256,
                },
            }
        ],
        "packages": packages,
        "files": files,
        "relationships": relationships,
    }


def generate_document(
    *,
    repo_root: pathlib.Path,
    version: str,
    source_sha: str,
    source_date_epoch: str,
    libraw_spdx: pathlib.Path,
    runtime_report: pathlib.Path,
) -> dict[str, object]:
    repo_root = repo_root.resolve()
    if repo_root.is_symlink() or not repo_root.is_dir():
        raise SpdxGenerationError(f"repo-root must be a directory: {repo_root}")
    version, source_sha = _validate_identity(version, source_sha)
    created = _created_at(source_date_epoch)
    lockfiles = _validate_lockfile_inventory(repo_root)
    dependencies = _parse_lockfiles(repo_root, lockfiles)
    # abspath preserves the final path component, allowing the regular-file
    # guard to reject a symlink rather than silently hashing its target.
    runtime_report = pathlib.Path(os.path.abspath(runtime_report))
    libraw_spdx = pathlib.Path(os.path.abspath(libraw_spdx))
    inputs = _collect_input_files(repo_root, lockfiles, runtime_report)
    libraw = _load_libraw_document(libraw_spdx)
    return _build_document(
        version=version,
        source_sha=source_sha,
        created=created,
        dependencies=dependencies,
        inputs=inputs,
        libraw=libraw,
    )


def _write_atomic(output: pathlib.Path, payload: bytes) -> None:
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            dir=output.parent,
            prefix=f".{output.name}.",
            suffix=".tmp",
            delete=False,
        ) as stream:
            temporary_name = stream.name
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_name, output)
        temporary_name = None
    finally:
        if temporary_name is not None:
            pathlib.Path(temporary_name).unlink(missing_ok=True)


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate a deterministic SPDX 2.3 application SBOM."
    )
    parser.add_argument("--repo-root", type=pathlib.Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--source-date-epoch", required=True)
    parser.add_argument("--libraw-spdx", type=pathlib.Path, required=True)
    parser.add_argument("--runtime-report", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _argument_parser().parse_args(argv)
    try:
        document = generate_document(
            repo_root=arguments.repo_root,
            version=arguments.version,
            source_sha=arguments.source_sha,
            source_date_epoch=arguments.source_date_epoch,
            libraw_spdx=arguments.libraw_spdx,
            runtime_report=arguments.runtime_report,
        )
        payload = (
            json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
        ).encode("utf-8")
        _write_atomic(arguments.output, payload)
    except (OSError, SpdxGenerationError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(f"wrote deterministic application SPDX: {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

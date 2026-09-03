#!/usr/bin/env python3
"""Create or verify the exact release-gate attestation.

SPDX-License-Identifier: GPL-3.0-only
"""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import re
import sys
import tempfile
from collections.abc import Sequence


GATES = (
    "engine-jni-boundary-assembly",
    "libraw-qualification",
    "native-safety-sanitizers",
    "o2-exact-engine-parity",
    "r8-and-16k-candidate",
    "release-instrumentation-assembly",
    "release-jvm-tests-and-lint",
    "shipping-exact-engine-parity",
    "unsigned-release-assembly",
)
TAG_RE = re.compile(r"v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)")
SHA_RE = re.compile(r"[0-9a-f]{40}")
POSITIVE_INTEGER_RE = re.compile(r"[1-9][0-9]*")


class GateError(ValueError):
    """A release gate input violated the fail-closed contract."""


def _regular_file(path: pathlib.Path, label: str) -> None:
    if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
        raise GateError(f"{label} must be a non-empty regular file: {path}")


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _validate_identity(version: str, source_sha: str, run_id: str, run_attempt: str) -> None:
    if TAG_RE.fullmatch(version) is None:
        raise GateError("version must be a stable vMAJOR.MINOR.PATCH tag")
    if SHA_RE.fullmatch(source_sha) is None:
        raise GateError("source-sha must be exactly 40 lowercase hexadecimal characters")
    if POSITIVE_INTEGER_RE.fullmatch(run_id) is None:
        raise GateError("run-id must be a positive decimal integer")
    if POSITIVE_INTEGER_RE.fullmatch(run_attempt) is None:
        raise GateError("run-attempt must be a positive decimal integer")


def _validate_markers(marker_dir: pathlib.Path) -> None:
    if marker_dir.is_symlink() or not marker_dir.is_dir():
        raise GateError(f"marker-dir must be a directory: {marker_dir}")
    actual = sorted(path.name for path in marker_dir.iterdir())
    if actual != list(GATES):
        missing = sorted(set(GATES) - set(actual))
        extra = sorted(set(actual) - set(GATES))
        raise GateError(f"gate marker inventory mismatch: missing={missing}; extra={extra}")
    for gate in GATES:
        marker = marker_dir / gate
        _regular_file(marker, f"gate marker {gate}")
        if marker.read_bytes() != b"PASS\n":
            raise GateError(f"gate marker is stale or not PASS: {gate}")


def expected_payload(
    *,
    version: str,
    source_sha: str,
    run_id: str,
    run_attempt: str,
    unsigned_apk: pathlib.Path,
    instrumentation_apk: pathlib.Path,
    engine_instrumentation_apk: pathlib.Path,
) -> bytes:
    _validate_identity(version, source_sha, run_id, run_attempt)
    _regular_file(unsigned_apk, "unsigned APK")
    _regular_file(instrumentation_apk, "instrumentation APK")
    _regular_file(engine_instrumentation_apk, "engine boundary instrumentation APK")
    lines = [
        "schema=spektrafilm-release-gates-v1",
        f"version={version}",
        f"release_sha={source_sha}",
        f"run_id={run_id}",
        f"run_attempt={run_attempt}",
        f"unsigned_apk_sha256={_sha256(unsigned_apk)}",
        f"instrumentation_apk_sha256={_sha256(instrumentation_apk)}",
        f"engine_instrumentation_apk_sha256={_sha256(engine_instrumentation_apk)}",
        *(f"gate.{gate}=PASS" for gate in GATES),
    ]
    return ("\n".join(lines) + "\n").encode("ascii")


def _write_atomic(output: pathlib.Path, payload: bytes) -> None:
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb", dir=output.parent, prefix=f".{output.name}.", delete=False
        ) as stream:
            temporary = stream.name
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, output)
        temporary = None
    finally:
        if temporary is not None:
            pathlib.Path(temporary).unlink(missing_ok=True)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--run-attempt", required=True)
    parser.add_argument("--unsigned-apk", type=pathlib.Path, required=True)
    parser.add_argument("--instrumentation-apk", type=pathlib.Path, required=True)
    parser.add_argument("--engine-instrumentation-apk", type=pathlib.Path, required=True)
    parser.add_argument("--attestation", type=pathlib.Path, required=True)
    parser.add_argument("--marker-dir", type=pathlib.Path)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        if arguments.action == "create":
            if arguments.marker_dir is None:
                raise GateError("create requires --marker-dir")
            _validate_markers(arguments.marker_dir)
        elif arguments.marker_dir is not None:
            raise GateError("verify does not accept --marker-dir")
        payload = expected_payload(
            version=arguments.version,
            source_sha=arguments.source_sha,
            run_id=arguments.run_id,
            run_attempt=arguments.run_attempt,
            unsigned_apk=arguments.unsigned_apk,
            instrumentation_apk=arguments.instrumentation_apk,
            engine_instrumentation_apk=arguments.engine_instrumentation_apk,
        )
        if arguments.action == "create":
            _write_atomic(arguments.attestation, payload)
        else:
            _regular_file(arguments.attestation, "gate attestation")
            if arguments.attestation.read_bytes() != payload:
                raise GateError("gate attestation is absent, stale, mutated, or hash-mismatched")
    except (OSError, GateError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(f"release gate attestation {arguments.action}: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

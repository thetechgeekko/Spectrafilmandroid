import hashlib
import importlib.util
import io
import json
import subprocess
import sys
import tarfile
import tempfile
import unittest
import warnings
import zipfile
from pathlib import Path
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / "libraw_bundle.py"
SPEC = importlib.util.spec_from_file_location("spektrafilm_libraw_bundle", SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load compliance tool: {SCRIPT}")
BUNDLE_TOOL = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = BUNDLE_TOOL
SPEC.loader.exec_module(BUNDLE_TOOL)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _tree_aggregate(files: dict[str, bytes]) -> str:
    audited = {
        path: data
        for path, data in files.items()
        if (path.startswith("src/") and path.endswith(".cpp"))
        or (path.startswith("libraw/") and path.endswith(".h"))
        or (path.startswith("internal/") and path.endswith(".h"))
    }
    manifest = "".join(
        f"{path}:{_sha256(audited[path])}\n" for path in sorted(audited)
    ).encode("ascii")
    return _sha256(manifest)


def _canonical_json(value: object) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def _mutate_json(path: Path, mutation) -> None:
    value = json.loads(path.read_bytes())
    mutation(value)
    path.write_bytes(_canonical_json(value))


def _rewrite_member_with_matching_manifest(
    source: Path, destination: Path, member: str, replacement: bytes
) -> None:
    with zipfile.ZipFile(source) as archive:
        infos = archive.infolist()
        payload = {info.filename: archive.read(info) for info in infos}
    if member not in payload or member == "manifest.json":
        raise AssertionError(f"invalid fixture rewrite member: {member}")
    payload[member] = replacement
    manifest = json.loads(payload["manifest.json"])
    for entry in manifest["files"]:
        if entry["path"] == member:
            entry["sha256"] = _sha256(replacement)
            entry["size"] = len(replacement)
            break
    else:
        raise AssertionError(f"manifest does not describe fixture member: {member}")
    payload["manifest.json"] = _canonical_json(manifest)
    with zipfile.ZipFile(destination, "w") as archive:
        for info in infos:
            archive.writestr(info, payload[info.filename])


def _fixture_pins() -> object:
    return BUNDLE_TOOL.Pins(
        version="1.2.3",
        url="https://example.invalid/source.tar.gz",
        archive_sha256="0" * 64,
        archive_size=1,
        tag_object="1" * 40,
        commit="2" * 40,
        patched_tree_sha256="3" * 64,
        patches=("fixture.patch",),
        patch_sha256={"fixture.patch": "4" * 64},
    )


def _write_tar(path: Path, members: list[tuple[str, bytes, str]]) -> None:
    with tarfile.open(path, "w:gz") as archive:
        for name, data, kind in members:
            info = tarfile.TarInfo(name)
            info.mtime = 0
            if kind == "file":
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
            elif kind == "symlink":
                info.type = tarfile.SYMTYPE
                info.linkname = "target"
                archive.addfile(info)
            elif kind == "fifo":
                info.type = tarfile.FIFOTYPE
                archive.addfile(info)
            else:
                raise AssertionError(f"unknown tar fixture kind: {kind}")


def _write_zip(
    path: Path,
    members: list[tuple[str, bytes]],
    *,
    compression: int = zipfile.ZIP_STORED,
    symlink: bool = False,
) -> None:
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", UserWarning)
        with zipfile.ZipFile(path, "w") as archive:
            for name, data in members:
                info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
                info.compress_type = compression
                info.create_system = 3
                info.external_attr = (0o120777 if symlink else 0o100644) << 16
                archive.writestr(info, data)


def _mark_zip_encrypted(path: Path) -> None:
    data = bytearray(path.read_bytes())
    for signature, flag_offset in ((b"PK\x03\x04", 6), (b"PK\x01\x02", 8)):
        start = 0
        while True:
            index = data.find(signature, start)
            if index < 0:
                break
            flags = int.from_bytes(data[index + flag_offset : index + flag_offset + 2], "little")
            data[index + flag_offset : index + flag_offset + 2] = (flags | 1).to_bytes(
                2, "little"
            )
            start = index + len(signature)
    path.write_bytes(data)


class FixtureRepository:
    def __init__(
        self,
        root: Path,
        route: str = "UNRESOLVED",
        ambiguous_offset: bool = False,
        patch_authorized: bool = True,
    ) -> None:
        self.root = root
        self.version = "1.2.3"
        self.offset_patch_name = "0000-add-prior-hardening-line.patch"
        self.patch_name = "0001-change-reviewed-line.patch"
        offset_old_start = 2 if ambiguous_offset else 1
        self.offset_patch = (
            "--- a/src/file000.cpp\n"
            "+++ b/src/file000.cpp\n"
            f"@@ -{offset_old_start} +{offset_old_start},2 @@\n"
            "+prior hardening line\n"
            " old reviewed line\n"
        ).encode()
        notice = (
            "// Modified by Spektrafilm Android contributors, 2026-08-30; see the\n"
            "// corresponding source distribution's bundled patch manifest.\n"
        )
        patch_parts: list[str] = []
        for index in range(BUNDLE_TOOL.MODIFIED_FILE_COUNT):
            old = "old reviewed line" if index == 0 else f"source {index}"
            new = "new reviewed line" if index == 0 else f"source {index}"
            relative = f"src/file{index:03d}.cpp"
            patch_parts.extend(
                (
                    f"--- a/{relative}\n",
                    f"+++ b/{relative}\n",
                    "@@ -1 +1,3 @@\n",
                    f"-{old}\n",
                    "+// Modified by Spektrafilm Android contributors, 2026-08-30; see the\n",
                    "+// corresponding source distribution's bundled patch manifest.\n",
                    f"+{new}\n",
                )
            )
        self.patch = "".join(patch_parts).encode("utf-8")

        upstream: dict[str, bytes] = {
            f"src/file{index:03d}.cpp": (
                b"old reviewed line\n" if index == 0 else f"source {index}\n".encode()
            )
            for index in range(98)
        }
        if ambiguous_offset:
            upstream["src/file000.cpp"] = (
                b"old reviewed line\nmiddle line\nold reviewed line\n"
            )
        upstream["libraw/libraw.h"] = b"fixture public header\n"
        upstream["internal/fixture.h"] = b"fixture internal header\n"
        upstream["COPYRIGHT"] = b"fixture copyright\n"
        upstream["LICENSE.LGPL"] = b"fixture LGPL text\n"
        upstream["LICENSE.CDDL"] = b"fixture CDDL text\n"
        upstream["README.md"] = b"fixture LibRaw source\n"

        patched = dict(upstream)
        for index in range(BUNDLE_TOOL.MODIFIED_FILE_COUNT):
            line = "new reviewed line\n" if index == 0 else f"source {index}\n"
            patched[f"src/file{index:03d}.cpp"] = (notice + line).encode("utf-8")
        if not ambiguous_offset:
            patched["src/file000.cpp"] = (
                "prior hardening line\n" + notice + "new reviewed line\n"
            ).encode("utf-8")
        aggregate = _tree_aggregate(patched)

        self.archive = root / "official.tar.gz"
        with tarfile.open(self.archive, "w:gz") as archive:
            for relative, data in sorted(upstream.items()):
                info = tarfile.TarInfo(f"LibRaw-{self.version}/{relative}")
                info.size = len(data)
                info.mode = 0o644
                info.mtime = 0
                archive.addfile(info, io.BytesIO(data))
        archive_bytes = self.archive.read_bytes()

        patch_dir = root / "lib/libraw/patches"
        patch_dir.mkdir(parents=True)
        (patch_dir / self.offset_patch_name).write_bytes(self.offset_patch)
        (patch_dir / self.patch_name).write_bytes(self.patch)
        markdown_marker = f"<!-- libraw-license-route: {route} -->"
        route_prose = f"LibRaw Android distribution route: {route}.\n"
        (patch_dir / "README.md").write_text(
            f"{markdown_marker}\n# Fixture ordered patch manifest\n{route_prose}",
            encoding="utf-8",
        )

        resolver_dir = root / "lib/libraw/cmake"
        resolver_dir.mkdir(parents=True)
        resolver = f'''set(SFRAW_PINNED_LIBRAW_VERSION "{self.version}")
set(SFRAW_PINNED_LIBRAW_URL
    "https://example.invalid/LibRaw-{self.version}.tar.gz")
set(SFRAW_PINNED_LIBRAW_SHA256
    "{_sha256(archive_bytes)}")
set(SFRAW_PINNED_LIBRAW_ARCHIVE_SIZE "{len(archive_bytes)}")
set(SFRAW_PINNED_LIBRAW_TAG_OBJECT
    "1111111111111111111111111111111111111111")
set(SFRAW_PINNED_LIBRAW_COMMIT
    "2222222222222222222222222222222222222222")
set(_SFRAW_LIBRAW_PATCHED_TREE_SHA256
    "{aggregate}")
set(_SFRAW_LIBRAW_PATCHES
    "${{_SFRAW_LIBRAW_PATCH_DIR}}/{self.offset_patch_name}"
    "${{_SFRAW_LIBRAW_PATCH_DIR}}/{self.patch_name}")
function(sfraw_resolve_libraw out_var)
    _sfraw_verify_file_sha256(
        "${{_SFRAW_LIBRAW_PATCH_DIR}}/{self.offset_patch_name}"
        "{_sha256(self.offset_patch)}"
        "fixture offset patch")
    _sfraw_verify_file_sha256(
        "${{_SFRAW_LIBRAW_PATCH_DIR}}/{self.patch_name}"
        "{_sha256(self.patch)}"
        "fixture patch")
endfunction()
'''
        (resolver_dir / "LibRawVendor.cmake").write_text(resolver, encoding="utf-8")

        compliance_dir = root / "lib/libraw/compliance"
        compliance_dir.mkdir(parents=True)
        (compliance_dir / "license-route.txt").write_text(route + "\n", encoding="ascii")
        selected = route != "UNRESOLVED"
        decision = {
            "approval_reference": (
                "https://example.invalid/issues/166#issuecomment-1" if selected else None
            ),
            "decision_date": "2026-08-30" if selected else None,
            "decision_owner": "Fixture human owner" if selected else None,
            "patch_contributions": {
                "authorization_reference": (
                    "https://example.invalid/issues/166#issuecomment-1"
                    if selected and patch_authorized
                    else None
                ),
                "license": route if selected else None,
                "rights_confirmed": selected and patch_authorized,
            },
            "rationale": (
                f"The owner selected the upstream {route} route for fixture distribution."
                if selected
                else None
            ),
            "recorded_at": "2026-08-30T00:00:00Z",
            "route": route,
            "schema": "spektrafilm.libraw-license-decision/v1",
        }
        self.decision_path = compliance_dir / "license-decision.json"
        self.decision_path.write_bytes(_canonical_json(decision))
        (compliance_dir / "spdx-created-at.txt").write_bytes(
            b"2026-08-30T00:01:00Z\n"
        )
        relink_dir = compliance_dir / "relink"
        relink_dir.mkdir()
        (relink_dir / "CMakeLists.txt").write_text(
            "set(LIBRAW_SOURCE_DIR required)\n"
            "add_library(raw STATIC)\n"
            "add_library(sfraw SHARED raw_decoder.cpp raw_decoder_jni.cpp "
            "unofficial_relink_marker.cpp)\n"
            "set_target_properties(sfraw PROPERTIES OUTPUT_NAME sfraw)\n"
            "target_compile_definitions(raw PRIVATE NO_JPEG USE_ZLIB LIBRAW_NODLL)\n"
            'target_link_options(sfraw PRIVATE "-Wl,-soname,libsfraw.so")\n'
            'target_link_options(sfraw PRIVATE "-Wl,-z,max-page-size=16384")\n',
            encoding="utf-8",
        )
        (relink_dir / "unofficial_relink_marker.cpp").write_text(
            'extern "C" const char *sfraw_recipient_relink_marker() { '
            'return "UNOFFICIAL RECIPIENT RELINK"; }\n',
            encoding="utf-8",
        )
        (relink_dir / "README.md").write_text(
            "# Recipient relink project\n", encoding="utf-8"
        )

        native_dir = root / "lib/libraw/src/main/cpp"
        native_dir.mkdir(parents=True)
        for name in (
            "CMakeLists.txt",
            "raw_decoder.cpp",
            "raw_decoder.h",
            "raw_decoder_jni.cpp",
        ):
            (native_dir / name).write_text(f"fixture {name}\n", encoding="utf-8")

        (root / "NOTICE.md").write_text(
            f"{markdown_marker}\nfixture notice\n{route_prose}", encoding="utf-8"
        )
        (root / "LICENSE").write_text("fixture GPL-3.0-only text\n", encoding="utf-8")
        (root / "lib/libraw/README.md").write_text(
            f"{markdown_marker}\nfixture LibRaw readme\n{route_prose}",
            encoding="utf-8",
        )
        docs = root / "docs"
        docs.mkdir()
        (docs / "LICENSING.md").write_text(
            f"{markdown_marker}\nfixture licensing\n{route_prose}", encoding="utf-8"
        )
        (docs / "PRODUCTION_READINESS_PLAN.md").write_text(
            "fixture route-neutral production plan\n", encoding="utf-8"
        )
        (docs / "RELEASE_CHECKLIST.md").write_text(
            "fixture route-neutral release checklist\n", encoding="utf-8"
        )
        dependencies = docs / "dependencies"
        dependencies.mkdir()
        (dependencies / "LIBRAW.md").write_text(
            f"{markdown_marker}\nfixture dependency record\n{route_prose}",
            encoding="utf-8",
        )
        legal_assets = root / "app/src/main/assets/legal/spektrafilm"
        legal_assets.mkdir(parents=True)
        (legal_assets / "NOTICE.md").write_bytes((root / "NOTICE.md").read_bytes())
        (legal_assets / "LICENSE.GPL-3.0").write_bytes((root / "LICENSE").read_bytes())
        about = root / "app/src/main/java/com/spectrafilm/app"
        about.mkdir(parents=True)
        (about / "AboutScreen.kt").write_text(
            f'internal const val LIBRAW_DISTRIBUTION_ROUTE = "{route}"\n{route_prose}',
            encoding="utf-8",
        )
        about_test = root / "app/src/test/java/com/spectrafilm/app"
        about_test.mkdir(parents=True)
        (about_test / "AboutLegalNoticeTest.kt").write_text(
            f'assertEquals("{route}", LIBRAW_DISTRIBUTION_ROUTE)\n{route_prose}',
            encoding="utf-8",
        )
        kotlin_source = root / "lib/libraw/src/main/kotlin/com/spectrafilm/libraw"
        kotlin_source.mkdir(parents=True)
        for name in ("RawDecoder.kt", "RawCoilDecoder.kt"):
            (kotlin_source / name).write_text(f"fixture {name}\n", encoding="utf-8")


class LibRawBundleCliTest(unittest.TestCase):
    def run_cli(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(SCRIPT), *arguments],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_build_is_deterministic_and_unresolved_bundle_verifies(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = FixtureRepository(root)
            first = root / "first.zip"
            second = root / "second.zip"

            for output in (first, second):
                result = self.run_cli(
                    "build",
                    "--repo-root",
                    str(root),
                    "--archive",
                    str(fixture.archive),
                    "--output",
                    str(output),
                )
                self.assertEqual(0, result.returncode, result.stderr)

            self.assertEqual(first.read_bytes(), second.read_bytes())
            verified = self.run_cli(
                "verify",
                "--repo-root",
                str(root),
                "--bundle",
                str(first),
            )
            self.assertEqual(0, verified.returncode, verified.stderr)
            self.assertIn("verified", verified.stdout.lower())

            with zipfile.ZipFile(first) as bundle:
                self.assertEqual(
                    (
                        b"prior hardening line\n"
                        b"// Modified by Spektrafilm Android contributors, 2026-08-30; see the\n"
                        b"// corresponding source distribution's bundled patch manifest.\n"
                        b"new reviewed line\n"
                    ),
                    bundle.read("source/LibRaw-1.2.3/src/file000.cpp"),
                )
                manifest = json.loads(bundle.read("manifest.json"))
            self.assertEqual("UNRESOLVED", manifest["license_route"])
            self.assertEqual(
                _sha256(fixture.decision_path.read_bytes()),
                manifest["license_decision_sha256"],
            )
            self.assertEqual(100, manifest["libraw"]["audited_file_count"])

    def test_release_verification_rejects_unresolved_route(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = FixtureRepository(root)
            bundle = root / "bundle.zip"
            built = self.run_cli(
                "build",
                "--repo-root",
                str(root),
                "--archive",
                str(fixture.archive),
                "--output",
                str(bundle),
            )
            self.assertEqual(0, built.returncode, built.stderr)

            verified = self.run_cli(
                "verify",
                "--repo-root",
                str(root),
                "--bundle",
                str(bundle),
                "--require-resolved",
            )
            self.assertEqual(2, verified.returncode)
            self.assertIn("license route is UNRESOLVED", verified.stderr)
            self.assertIn("permits only LGPL-2.1-only", verified.stderr)

    def test_release_verification_accepts_only_lgpl_static_route(self) -> None:
        cases = (
            ("LGPL-2.1-only", 0, "verified"),
            ("CDDL-1.0", 2, "statically combined with GPL application code"),
        )
        for route, expected_code, expected_text in cases:
            with self.subTest(route=route), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                fixture = FixtureRepository(root, route=route)
                bundle = root / "bundle.zip"
                built = self.run_cli(
                    "build",
                    "--repo-root",
                    str(root),
                    "--archive",
                    str(fixture.archive),
                    "--output",
                    str(bundle),
                )
                self.assertEqual(0, built.returncode, built.stderr)

                verified = self.run_cli(
                    "verify",
                    "--repo-root",
                    str(root),
                    "--bundle",
                    str(bundle),
                    "--require-resolved",
                )
                self.assertEqual(expected_code, verified.returncode)
                output = verified.stdout + verified.stderr
                self.assertIn(expected_text, output)

    def test_audit_route_rejects_marker_only_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            FixtureRepository(root)
            marker = root / "lib/libraw/compliance/license-route.txt"
            marker.write_text("LGPL-2.1-only\n", encoding="ascii")

            audited = self.run_cli("audit-route", "--repo-root", str(root))
            self.assertEqual(2, audited.returncode)
            self.assertIn("decision route does not match", audited.stderr)

    def test_audit_route_rejects_non_pending_unresolved_record(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = FixtureRepository(root)
            _mutate_json(
                fixture.decision_path,
                lambda record: record.update({"decision_owner": "Premature owner"}),
            )

            audited = self.run_cli("audit-route", "--repo-root", str(root))
            self.assertEqual(2, audited.returncode)
            self.assertIn("clean pending decision record", audited.stderr)

    def test_audit_route_rejects_noncanonical_or_future_spdx_timestamp(self) -> None:
        cases = (
            (b"2999-01-01T00:00:00Z\n", "must not be in the future"),
            (
                b"2026-08-30T00:01:00Z\r\n",
                "one canonical newline-terminated line",
            ),
        )
        for replacement, expected in cases:
            with self.subTest(expected=expected), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                FixtureRepository(root)
                (root / "lib/libraw/compliance/spdx-created-at.txt").write_bytes(
                    replacement
                )
                audited = self.run_cli("audit-route", "--repo-root", str(root))
                self.assertEqual(2, audited.returncode)
                self.assertIn(expected, audited.stderr)

    def test_audit_route_rejects_spdx_timestamp_before_decision_record(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            FixtureRepository(root, route="LGPL-2.1-only")
            (root / "lib/libraw/compliance/spdx-created-at.txt").write_bytes(
                b"2026-08-29T23:59:59Z\n"
            )

            audited = self.run_cli(
                "audit-route",
                "--repo-root",
                str(root),
                "--require-resolved",
            )
            self.assertEqual(2, audited.returncode)
            self.assertIn(
                "SPDX source timestamp must not precede the license decision record",
                audited.stderr,
            )

    def test_release_audit_requires_patch_contribution_authorization(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            FixtureRepository(root, route="LGPL-2.1-only", patch_authorized=False)

            ordinary = self.run_cli("audit-route", "--repo-root", str(root))
            self.assertEqual(2, ordinary.returncode)
            self.assertIn("rights confirmation", ordinary.stderr)
            release = self.run_cli(
                "audit-route",
                "--repo-root",
                str(root),
                "--require-resolved",
            )
            self.assertEqual(2, release.returncode)
            self.assertIn("rights confirmation", release.stderr)

    def test_selected_cddl_route_also_requires_human_authorization(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            FixtureRepository(root, route="CDDL-1.0", patch_authorized=False)
            audited = self.run_cli("audit-route", "--repo-root", str(root))
            self.assertEqual(2, audited.returncode)
            self.assertIn("rights confirmation", audited.stderr)

    def test_release_audit_rejects_stale_unresolved_prose(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            FixtureRepository(root, route="LGPL-2.1-only")
            licensing = root / "docs/LICENSING.md"
            licensing.write_text(
                licensing.read_text(encoding="utf-8") + "stale unresolved route text\n",
                encoding="utf-8",
            )

            audited = self.run_cli(
                "audit-route",
                "--repo-root",
                str(root),
                "--require-resolved",
            )
            self.assertEqual(2, audited.returncode)
            self.assertIn("stale unresolved-route prose", audited.stderr)

    def test_release_audit_rejects_contradictory_human_route_claim(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            FixtureRepository(root, route="LGPL-2.1-only")
            licensing = root / "docs/LICENSING.md"
            licensing.write_text(
                licensing.read_text(encoding="utf-8")
                + "The selected LibRaw distribution route is CDDL-1.0.\n",
                encoding="utf-8",
            )

            audited = self.run_cli(
                "audit-route",
                "--repo-root",
                str(root),
                "--require-resolved",
            )
            self.assertEqual(2, audited.returncode)
            self.assertIn("human-facing LibRaw route claim", audited.stderr)

    def test_audit_route_requires_human_route_claim_on_every_surface(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            FixtureRepository(root)
            readme = root / "lib/libraw/README.md"
            readme.write_text(
                readme.read_text(encoding="utf-8").replace(
                    "LibRaw Android distribution route: UNRESOLVED.\n", ""
                ),
                encoding="utf-8",
            )

            audited = self.run_cli("audit-route", "--repo-root", str(root))
            self.assertEqual(2, audited.returncode)
            self.assertIn("human-facing LibRaw route claim", audited.stderr)
            self.assertIn("lib/libraw/README.md", audited.stderr)

    def test_route_audit_rejects_offline_legal_asset_drift(self) -> None:
        cases = (
            (
                "app/src/main/assets/legal/spektrafilm/NOTICE.md",
                "offline app NOTICE asset differs",
            ),
            (
                "app/src/main/assets/legal/spektrafilm/LICENSE.GPL-3.0",
                "offline app GPL asset differs",
            ),
        )
        for relative, expected in cases:
            with self.subTest(relative=relative), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                FixtureRepository(root)
                path = root / relative
                path.write_text(path.read_text(encoding="utf-8") + "drift\n", encoding="utf-8")
                audited = self.run_cli("audit-route", "--repo-root", str(root))
                self.assertEqual(2, audited.returncode)
                self.assertIn(expected, audited.stderr)

    def test_release_audit_requires_https_references_and_ordered_dates(self) -> None:
        cases = (
            (
                lambda record: record.update({"approval_reference": "issue-166"}),
                "approval reference must be an HTTPS URL",
            ),
            (
                lambda record: record.update({"recorded_at": "2026-08-29T00:00:00Z"}),
                "recorded_at date must not precede decision_date",
            ),
            (
                lambda record: record.update({"recorded_at": "2999-01-01T00:00:00Z"}),
                "recorded_at must not be in the future",
            ),
            (
                lambda record: record.update(
                    {"recorded_at": "2026-08-30T00:00:00+00:00"}
                ),
                "literal YYYY-MM-DDTHH:MM:SSZ",
            ),
        )
        for mutation, expected in cases:
            with self.subTest(expected=expected), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                fixture = FixtureRepository(root, route="LGPL-2.1-only")
                _mutate_json(fixture.decision_path, mutation)
                audited = self.run_cli(
                    "audit-route",
                    "--repo-root",
                    str(root),
                    "--require-resolved",
                )
                self.assertEqual(2, audited.returncode)
                self.assertIn(expected, audited.stderr)

    def test_build_rejects_archive_and_output_aliases_without_overwrite(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = FixtureRepository(root)
            original_archive = fixture.archive.read_bytes()

            archive_alias = self.run_cli(
                "build",
                "--repo-root",
                str(root),
                "--archive",
                str(fixture.archive),
                "--output",
                str(fixture.archive),
            )
            self.assertEqual(2, archive_alias.returncode)
            self.assertIn("must resolve to distinct files", archive_alias.stderr)
            self.assertEqual(original_archive, fixture.archive.read_bytes())

            same_destination = root / "same.zip"
            output_alias = self.run_cli(
                "build",
                "--repo-root",
                str(root),
                "--archive",
                str(fixture.archive),
                "--output",
                str(same_destination),
                "--sbom-output",
                str(same_destination),
            )
            self.assertEqual(2, output_alias.returncode)
            self.assertIn("--output and --sbom-output", output_alias.stderr)
            self.assertFalse(same_destination.exists())

    def test_bundle_contains_standalone_recipient_relink_project(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = FixtureRepository(root)
            relink_dir = root / "lib/libraw/compliance/relink"
            (relink_dir / "CMakeLists.txt").write_text(
                "set(LIBRAW_SOURCE_DIR required)\n"
                "add_library(raw STATIC)\n"
                "add_library(sfraw SHARED raw_decoder.cpp raw_decoder_jni.cpp "
                "unofficial_relink_marker.cpp)\n"
                "set_target_properties(sfraw PROPERTIES OUTPUT_NAME sfraw)\n"
                "target_compile_definitions(raw PRIVATE NO_JPEG USE_ZLIB LIBRAW_NODLL)\n"
                'target_link_options(sfraw PRIVATE "-Wl,-soname,libsfraw.so")\n'
                'target_link_options(sfraw PRIVATE "-Wl,-z,max-page-size=16384")\n',
                encoding="utf-8",
            )
            (relink_dir / "unofficial_relink_marker.cpp").write_text(
                'extern "C" const char *sfraw_recipient_relink_marker() { '
                'return "UNOFFICIAL RECIPIENT RELINK"; }\n',
                encoding="utf-8",
            )
            (relink_dir / "README.md").write_text(
                "# Recipient relink project\n", encoding="utf-8"
            )
            bundle = root / "bundle.zip"

            built = self.run_cli(
                "build",
                "--repo-root",
                str(root),
                "--archive",
                str(fixture.archive),
                "--output",
                str(bundle),
            )
            self.assertEqual(0, built.returncode, built.stderr)
            with zipfile.ZipFile(bundle) as archive:
                cmake = archive.read(
                    "relink/lib/libraw/compliance/relink/CMakeLists.txt"
                ).decode("utf-8")
                marker = archive.read(
                    "relink/lib/libraw/compliance/relink/unofficial_relink_marker.cpp"
                ).decode("utf-8")
                relinking = archive.read("RELINKING.md").decode("utf-8")
            self.assertIn("LIBRAW_SOURCE_DIR", cmake)
            self.assertIn("add_library(raw STATIC", cmake)
            self.assertIn("add_library(sfraw SHARED", cmake)
            self.assertIn("max-page-size=16384", cmake)
            self.assertIn("UNOFFICIAL RECIPIENT RELINK", marker)
            self.assertIn("-DANDROID_STL=c++_shared", relinking)

    def test_spdx_identity_changes_with_exact_wrapper_sources(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = FixtureRepository(root)
            first = root / "first.zip"
            second = root / "second.zip"

            built = self.run_cli(
                "build",
                "--repo-root",
                str(root),
                "--archive",
                str(fixture.archive),
                "--output",
                str(first),
            )
            self.assertEqual(0, built.returncode, built.stderr)
            wrapper_source = root / "lib/libraw/src/main/cpp/raw_decoder.cpp"
            wrapper_source.write_text(
                wrapper_source.read_text(encoding="utf-8") + "reviewed change\n",
                encoding="utf-8",
            )
            rebuilt = self.run_cli(
                "build",
                "--repo-root",
                str(root),
                "--archive",
                str(fixture.archive),
                "--output",
                str(second),
            )
            self.assertEqual(0, rebuilt.returncode, rebuilt.stderr)

            with zipfile.ZipFile(first) as archive:
                first_spdx = json.loads(archive.read("sbom.spdx.json"))
            with zipfile.ZipFile(second) as archive:
                second_spdx = json.loads(archive.read("sbom.spdx.json"))
            first_wrapper = next(
                package
                for package in first_spdx["packages"]
                if package["SPDXID"] == "SPDXRef-Package-sfraw-wrapper"
            )
            second_wrapper = next(
                package
                for package in second_spdx["packages"]
                if package["SPDXID"] == "SPDXRef-Package-sfraw-wrapper"
            )
            self.assertNotEqual(
                first_wrapper["packageVerificationCode"],
                second_wrapper["packageVerificationCode"],
            )
            self.assertNotEqual(
                first_spdx["documentNamespace"], second_spdx["documentNamespace"]
            )

    def test_patch_engine_rejects_equal_distance_exact_context_tie(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = FixtureRepository(root, ambiguous_offset=True)
            result = self.run_cli(
                "build",
                "--repo-root",
                str(root),
                "--archive",
                str(fixture.archive),
                "--output",
                str(root / "bundle.zip"),
            )
            self.assertEqual(2, result.returncode)
            self.assertIn(fixture.offset_patch_name, result.stderr)
            self.assertIn("equal-distance ambiguity", result.stderr)

    def test_tar_parser_rejects_path_link_duplicate_and_ceiling_attacks(self) -> None:
        path_cases = (
            "LibRaw-1.2.3/../escape",
            "/LibRaw-1.2.3/escape",
            "LibRaw-1.2.3\\escape",
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pins = _fixture_pins()
            for index, name in enumerate(path_cases):
                with self.subTest(path=name):
                    archive = root / f"path-{index}.tar.gz"
                    _write_tar(archive, [(name, b"x", "file")])
                    with self.assertRaisesRegex(BUNDLE_TOOL.BundleError, "unsafe archive"):
                        BUNDLE_TOOL._extract_archive(archive, root / f"out-{index}", pins)

            for kind in ("symlink", "fifo"):
                with self.subTest(kind=kind):
                    archive = root / f"{kind}.tar.gz"
                    _write_tar(
                        archive,
                        [(f"LibRaw-1.2.3/{kind}", b"", kind)],
                    )
                    with self.assertRaisesRegex(
                        BUNDLE_TOOL.BundleError, "unsupported link or special member"
                    ):
                        BUNDLE_TOOL._extract_archive(archive, root / f"out-{kind}", pins)

            duplicate = root / "duplicate.tar.gz"
            duplicate_name = "LibRaw-1.2.3/duplicate"
            _write_tar(
                duplicate,
                [(duplicate_name, b"a", "file"), (duplicate_name, b"b", "file")],
            )
            with self.assertRaisesRegex(BUNDLE_TOOL.BundleError, "duplicate file"):
                BUNDLE_TOOL._extract_archive(duplicate, root / "out-duplicate", pins)

            count = root / "count.tar.gz"
            _write_tar(
                count,
                [
                    ("LibRaw-1.2.3/one", b"1", "file"),
                    ("LibRaw-1.2.3/two", b"2", "file"),
                ],
            )
            with mock.patch.object(
                BUNDLE_TOOL, "MAX_ARCHIVE_FILES", 1
            ), self.assertRaisesRegex(BUNDLE_TOOL.BundleError, "file-count safety"):
                BUNDLE_TOOL._extract_archive(count, root / "out-count", pins)

            size = root / "size.tar.gz"
            _write_tar(size, [("LibRaw-1.2.3/large", b"12", "file")])
            with mock.patch.object(
                BUNDLE_TOOL, "MAX_ARCHIVE_BYTES", 1
            ), self.assertRaisesRegex(BUNDLE_TOOL.BundleError, "extracted-byte safety"):
                BUNDLE_TOOL._extract_archive(size, root / "out-size", pins)

    def test_zip_parser_rejects_path_link_duplicate_encryption_compression_and_ceilings(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for index, name in enumerate(("../escape", "/absolute", "back\\slash")):
                with self.subTest(path=name):
                    bundle = root / f"path-{index}.zip"
                    _write_zip(bundle, [(name, b"x")])
                    if "\\" in name:
                        # ZipInfo normalizes backslashes on Windows. Restore the
                        # malicious byte sequence in both local and central names.
                        data = bundle.read_bytes().replace(b"back/slash", b"back\\slash")
                        bundle.write_bytes(data)
                    with self.assertRaisesRegex(BUNDLE_TOOL.BundleError, "unsafe ZIP"):
                        BUNDLE_TOOL._read_bundle_members(bundle)

            symlink = root / "symlink.zip"
            _write_zip(symlink, [("link", b"target")], symlink=True)
            with self.assertRaisesRegex(BUNDLE_TOOL.BundleError, "non-regular ZIP member"):
                BUNDLE_TOOL._read_bundle_members(symlink)

            duplicate = root / "duplicate.zip"
            _write_zip(duplicate, [("same", b"a"), ("same", b"b")])
            with self.assertRaisesRegex(BUNDLE_TOOL.BundleError, "duplicate member"):
                BUNDLE_TOOL._read_bundle_members(duplicate)

            encrypted = root / "encrypted.zip"
            _write_zip(encrypted, [("member", b"x")])
            _mark_zip_encrypted(encrypted)
            with self.assertRaisesRegex(BUNDLE_TOOL.BundleError, "encrypted ZIP member"):
                BUNDLE_TOOL._read_bundle_members(encrypted)

            stored = root / "deflated.zip"
            _write_zip(stored, [("member", b"x")], compression=zipfile.ZIP_DEFLATED)
            with self.assertRaisesRegex(BUNDLE_TOOL.BundleError, "unexpected ZIP compression"):
                BUNDLE_TOOL._read_bundle_members(stored)

            count = root / "count.zip"
            _write_zip(count, [("one", b"1"), ("two", b"2")])
            with mock.patch.object(
                BUNDLE_TOOL, "MAX_BUNDLE_FILES", 1
            ), self.assertRaisesRegex(BUNDLE_TOOL.BundleError, "file-count safety"):
                BUNDLE_TOOL._read_bundle_members(count)

            size = root / "size.zip"
            _write_zip(size, [("large", b"12")])
            with mock.patch.object(
                BUNDLE_TOOL, "MAX_BUNDLE_BYTES", 1
            ), self.assertRaisesRegex(
                BUNDLE_TOOL.BundleError, "uncompressed-byte safety"
            ):
                BUNDLE_TOOL._read_bundle_members(size)

            global_comment = root / "global-comment.zip"
            _write_zip(global_comment, [("member", b"x")])
            with zipfile.ZipFile(global_comment, "a") as archive:
                archive.comment = b"forbidden"
            with self.assertRaisesRegex(BUNDLE_TOOL.BundleError, "global comment"):
                BUNDLE_TOOL._read_bundle_members(global_comment)

            metadata_cases = (
                ("extra", lambda info: setattr(info, "extra", b"\x01\x00\x00\x00"), "extra/comment"),
                ("comment", lambda info: setattr(info, "comment", b"x"), "extra/comment"),
                (
                    "mode",
                    lambda info: setattr(info, "external_attr", 0o100600 << 16),
                    "mode is not canonical",
                ),
                (
                    "version",
                    lambda info: setattr(info, "create_version", 21),
                    "version/attribute metadata",
                ),
            )
            for label, mutate, expected in metadata_cases:
                with self.subTest(metadata=label):
                    bundle = root / f"metadata-{label}.zip"
                    info = zipfile.ZipInfo("member", (1980, 1, 1, 0, 0, 0))
                    info.compress_type = zipfile.ZIP_STORED
                    info.create_system = 3
                    info.create_version = 20
                    info.extract_version = 20
                    info.external_attr = 0o100644 << 16
                    mutate(info)
                    with zipfile.ZipFile(bundle, "w") as archive:
                        archive.writestr(info, b"x")
                    with self.assertRaisesRegex(BUNDLE_TOOL.BundleError, expected):
                        BUNDLE_TOOL._read_bundle_members(bundle)

    def test_bundle_carries_exact_archive_inventory_notices_and_sidecar_sbom(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = FixtureRepository(root)
            bundle_path = root / "bundle.zip"
            sidecar = root / "bundle.spdx.json"
            built = self.run_cli(
                "build",
                "--repo-root",
                str(root),
                "--archive",
                str(fixture.archive),
                "--output",
                str(bundle_path),
                "--sbom-output",
                str(sidecar),
            )
            self.assertEqual(0, built.returncode, built.stderr)

            with zipfile.ZipFile(bundle_path) as bundle:
                pristine = bundle.read("source/LibRaw-1.2.3.tar.gz")
                modifications = bundle.read("MODIFICATIONS.md").decode("utf-8")
                sbom_bytes = bundle.read("sbom.spdx.json")
                self.assertEqual(
                    (root / "lib/libraw/patches/README.md").read_bytes(),
                    bundle.read("relink/lib/libraw/patches/README.md"),
                )
                self.assertEqual(
                    (root / "NOTICE.md").read_bytes(),
                    bundle.read("release/NOTICE.md"),
                )
                self.assertEqual(
                    (root / "docs/LICENSING.md").read_bytes(),
                    bundle.read("release/docs/LICENSING.md"),
                )
                self.assertEqual(
                    (root / "LICENSE").read_bytes(),
                    bundle.read("release/LICENSE.GPL-3.0"),
                )
                self.assertEqual(
                    fixture.decision_path.read_bytes(),
                    bundle.read("relink/lib/libraw/compliance/license-decision.json"),
                )
                self.assertEqual(
                    b"2026-08-30T00:01:00Z\n",
                    bundle.read("relink/lib/libraw/compliance/spdx-created-at.txt"),
                )
            self.assertEqual(fixture.archive.read_bytes(), pristine)
            self.assertEqual(sbom_bytes, sidecar.read_bytes())
            sbom = json.loads(sbom_bytes)
            wrapper = next(
                package
                for package in sbom["packages"]
                if package["SPDXID"] == "SPDXRef-Package-sfraw-wrapper"
            )
            self.assertEqual("GPL-3.0-only", wrapper["licenseDeclared"])
            self.assertEqual("GPL-3.0-only", wrapper["licenseConcluded"])
            self.assertNotIn("versionInfo", wrapper)
            self.assertEqual(
                "2026-08-30T00:01:00Z", sbom["creationInfo"]["created"]
            )
            self.assertRegex(
                sbom["documentNamespace"], r"/UNRESOLVED/[0-9a-f]{64}$"
            )
            self.assertTrue(wrapper["filesAnalyzed"])
            self.assertEqual(7, len(wrapper["hasFiles"]))
            self.assertRegex(
                wrapper["packageVerificationCode"]["packageVerificationCodeValue"],
                r"^[0-9a-f]{40}$",
            )
            pristine = next(
                package
                for package in sbom["packages"]
                if package["SPDXID"] == "SPDXRef-Package-LibRaw-Pristine"
            )
            patched = next(
                package
                for package in sbom["packages"]
                if package["SPDXID"] == "SPDXRef-Package-LibRaw-Patched"
            )
            self.assertFalse(pristine["filesAnalyzed"])
            self.assertEqual(
                _sha256(fixture.archive.read_bytes()),
                pristine["checksums"][0]["checksumValue"],
            )
            self.assertTrue(patched["filesAnalyzed"])
            self.assertGreater(len(patched["hasFiles"]), 100)
            relationship_types = {
                relationship["relationshipType"]
                for relationship in sbom["relationships"]
            }
            self.assertIn("GENERATED_FROM", relationship_types)
            self.assertIn("PATCH_APPLIED", relationship_types)
            self.assertIn("STATIC_LINK", relationship_types)
            self.assertNotIn("or-later", sbom_bytes.decode("utf-8"))

            self.assertIn(f"Modification date: `{BUNDLE_TOOL.MODIFICATION_DATE}`", modifications)
            self.assertIn(
                "Contributor: `Spektrafilm Android contributors`", modifications
            )
            self.assertIn(f"## Modified upstream files ({BUNDLE_TOOL.MODIFIED_FILE_COUNT})", modifications)
            modified_lines = [
                line
                for line in modifications.splitlines()
                if line.startswith("- `src/file")
            ]
            self.assertEqual(
                [f"- `src/file{index:03d}.cpp`" for index in range(BUNDLE_TOOL.MODIFIED_FILE_COUNT)],
                modified_lines,
            )
            self.assertLess(
                modifications.index(f"1. `{fixture.offset_patch_name}`"),
                modifications.index(f"2. `{fixture.patch_name}`"),
            )

    def test_semantic_verifier_rejects_tamper_even_with_updated_manifest(self) -> None:
        cases = (
            ("source/LibRaw-1.2.3.tar.gz", b"x", "archive size mismatch"),
            (
                "source/LibRaw-1.2.3/README.md",
                b"changed non-audited upstream file\n",
                "differs from authenticated archive plus pinned patches",
            ),
            ("MODIFICATIONS.md", b"incomplete\n", "exact modified-file inventory"),
            (
                "relink/lib/libraw/patches/README.md",
                b"changed patch manifest\n",
                "patch manifest differs from repository",
            ),
            (
                "relink/lib/libraw/compliance/relink/CMakeLists.txt",
                b"changed relink project\n",
                "standalone recipient relink file",
            ),
            ("release/NOTICE.md", b"changed notice\n", "NOTICE.md differs"),
            (
                "release/LICENSE.GPL-3.0",
                b"changed project license\n",
                "GPL-3.0-only license differs",
            ),
            (
                "release/docs/LICENSING.md",
                b"changed licensing\n",
                "docs/LICENSING.md differs",
            ),
            (
                "relink/lib/libraw/compliance/license-decision.json",
                b"{}\n",
                "license decision has unexpected fields",
            ),
            (
                "relink/lib/libraw/compliance/spdx-created-at.txt",
                b"2026-08-30T00:02:00Z\n",
                "bundled SPDX source timestamp differs",
            ),
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = FixtureRepository(root)
            original = root / "original.zip"
            built = self.run_cli(
                "build",
                "--repo-root",
                str(root),
                "--archive",
                str(fixture.archive),
                "--output",
                str(original),
            )
            self.assertEqual(0, built.returncode, built.stderr)

            for index, (member, replacement, expected) in enumerate(cases):
                with self.subTest(member=member):
                    tampered = root / f"tampered-{index}.zip"
                    _rewrite_member_with_matching_manifest(
                        original, tampered, member, replacement
                    )
                    verified = self.run_cli(
                        "verify",
                        "--repo-root",
                        str(root),
                        "--bundle",
                        str(tampered),
                    )
                    self.assertEqual(2, verified.returncode)
                    self.assertIn(expected, verified.stderr)


if __name__ == "__main__":
    unittest.main()

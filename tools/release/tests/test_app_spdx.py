"""Black-box tests for the deterministic application SPDX generator."""

from __future__ import annotations

import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "tools" / "release" / "app_spdx.py"
EXPECTED_LOCKFILES = (
    "settings-gradle.lockfile",
    "app/gradle.lockfile",
    "engine/spektra-core/gradle.lockfile",
    "lib/libraw/gradle.lockfile",
    "lib/pngwriter/gradle.lockfile",
    "lib/tiffwriter/gradle.lockfile",
)
SOURCE_SHA = "0123456789abcdef0123456789abcdef01234567"
SOURCE_DATE_EPOCH = "1700000000"


class AppSpdxCliTest(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.repo = pathlib.Path(self._temp.name) / "repo"
        self.repo.mkdir()

        lock_contents = {
            "settings-gradle.lockfile": "empty=incomingCatalogForLibs0\n",
            "app/gradle.lockfile": (
                "com.example:alpha:1.0=releaseRuntimeClasspath\n"
                "org.demo:ui:3.1=releaseRuntimeClasspath\n"
            ),
            "engine/spektra-core/gradle.lockfile": (
                "com.example:alpha:1.0=releaseCompileClasspath\n"
            ),
            # A second version in an isolated configuration is legitimate and
            # must be represented as a separate GAV/package, not rejected.
            "lib/libraw/gradle.lockfile": (
                "com.example:alpha:2.0=debugCompileClasspath\n"
            ),
            # Gradle emits this exact sentinel when no configurations are empty.
            "lib/pngwriter/gradle.lockfile": "empty=\n",
            "lib/tiffwriter/gradle.lockfile": "empty=releaseRuntimeClasspath\n",
        }
        for relative, content in lock_contents.items():
            self._write(relative, content.encode("utf-8"))

        self._write(
            "gradle/libs.versions.toml",
            b'[versions]\nkotlin = "2.0.21"\n',
        )
        self._write(
            "gradle/wrapper/gradle-wrapper.properties",
            b"distributionUrl=https\\://services.gradle.org/distributions/gradle-8.14.3-bin.zip\n",
        )
        self._write("gradle/wrapper/gradle-wrapper.jar", b"fixture-wrapper-jar\x00\xff")
        self.runtime_report = self.repo / "build" / "release-runtime-classpath.txt"
        self._write(
            "build/release-runtime-classpath.txt",
            b"releaseRuntimeClasspath\n+--- com.example:alpha:1.0\n",
        )

        self.libraw_spdx = self.repo / "build" / "libraw.spdx.json"
        wrapper_file_sha1 = hashlib.sha1(b"fixture wrapper file").hexdigest()
        wrapper_verification_code = hashlib.sha1(
            wrapper_file_sha1.encode("ascii")
        ).hexdigest()
        libraw_document = {
            "SPDXID": "SPDXRef-DOCUMENT",
            "spdxVersion": "SPDX-2.3",
            "dataLicense": "CC0-1.0",
            "name": "LibRaw fixture",
            "documentNamespace": (
                "https://example.test/spdx/libraw/0.22.2/fixture"
            ),
            "creationInfo": {
                "created": "2023-11-14T22:13:20Z",
                "creators": ["Tool: fixture"],
            },
            "packages": [
                {
                    "SPDXID": "SPDXRef-Package-sfraw-wrapper",
                    "name": "sfraw wrapper",
                    "downloadLocation": "NOASSERTION",
                    "filesAnalyzed": True,
                    "licenseConcluded": "NOASSERTION",
                    "licenseDeclared": "NOASSERTION",
                    "copyrightText": "NOASSERTION",
                    "hasFiles": ["SPDXRef-File-wrapper-fixture"],
                    "packageVerificationCode": {
                        "packageVerificationCodeValue": wrapper_verification_code
                    },
                }
            ],
            "files": [
                {
                    "SPDXID": "SPDXRef-File-wrapper-fixture",
                    "fileName": "wrapper/fixture.cpp",
                    "checksums": [
                        {"algorithm": "SHA1", "checksumValue": wrapper_file_sha1}
                    ],
                    "licenseConcluded": "NOASSERTION",
                    "licenseInfoInFiles": ["NOASSERTION"],
                    "copyrightText": "NOASSERTION",
                }
            ],
            "relationships": [],
        }
        self._write(
            "build/libraw.spdx.json",
            (json.dumps(libraw_document, sort_keys=True) + "\n").encode("utf-8"),
        )

    def _write(self, relative: str, data: bytes) -> pathlib.Path:
        path = self.repo / pathlib.PurePosixPath(relative)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def _run(
        self,
        output_name: str,
        *,
        libraw_spdx: pathlib.Path | None = None,
        runtime_report: pathlib.Path | None = None,
        version: str = "v0.9.0",
        source_sha: str = SOURCE_SHA,
    ) -> tuple[subprocess.CompletedProcess[str], pathlib.Path]:
        output = pathlib.Path(self._temp.name) / output_name
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--repo-root",
                str(self.repo),
                "--version",
                version,
                "--source-sha",
                source_sha,
                "--source-date-epoch",
                SOURCE_DATE_EPOCH,
                "--libraw-spdx",
                str(libraw_spdx or self.libraw_spdx),
                "--runtime-report",
                str(runtime_report or self.runtime_report),
                "--output",
                str(output),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        return result, output

    def _assert_failed(self, result: subprocess.CompletedProcess[str], output: pathlib.Path) -> None:
        self.assertNotEqual(0, result.returncode, result.stdout)
        self.assertFalse(output.exists(), "failure must not leave a partial SPDX document")

    def test_output_is_deterministic_and_inventories_all_inputs(self) -> None:
        first_result, first = self._run("first.spdx.json")
        second_result, second = self._run("second.spdx.json")
        self.assertEqual(0, first_result.returncode, first_result.stderr)
        self.assertEqual(0, second_result.returncode, second_result.stderr)
        self.assertEqual(first.read_bytes(), second.read_bytes())

        document = json.loads(first.read_text(encoding="utf-8"))
        self.assertEqual("SPDX-2.3", document["spdxVersion"])
        self.assertEqual("CC0-1.0", document["dataLicense"])
        self.assertEqual(
            "2023-11-14T22:13:20Z",
            document["creationInfo"]["created"],
        )
        self.assertTrue(
            document["documentNamespace"].startswith(
                "https://github.com/thetechgeekko/Spektrafilm-android/spdx/app/"
            )
        )

        expected_files = {
            *(f"./{path}" for path in EXPECTED_LOCKFILES),
            "./gradle/libs.versions.toml",
            "./gradle/wrapper/gradle-wrapper.properties",
            "./gradle/wrapper/gradle-wrapper.jar",
            "./release-runtime-classpath.txt",
        }
        files = {item["fileName"]: item for item in document["files"]}
        self.assertEqual(expected_files, set(files))
        expected_input_paths = {
            **{
                f"./{relative}": self.repo / pathlib.PurePosixPath(relative)
                for relative in EXPECTED_LOCKFILES
            },
            "./gradle/libs.versions.toml": self.repo / "gradle/libs.versions.toml",
            "./gradle/wrapper/gradle-wrapper.properties": (
                self.repo / "gradle/wrapper/gradle-wrapper.properties"
            ),
            "./gradle/wrapper/gradle-wrapper.jar": (
                self.repo / "gradle/wrapper/gradle-wrapper.jar"
            ),
            "./release-runtime-classpath.txt": self.runtime_report,
        }
        for display_name, input_path in expected_input_paths.items():
            expected_sha = hashlib.sha256(input_path.read_bytes()).hexdigest()
            self.assertEqual(
                [{"algorithm": "SHA256", "checksumValue": expected_sha}],
                files[display_name]["checksums"],
                display_name,
            )

        packages = {item["SPDXID"]: item for item in document["packages"]}
        app = packages["SPDXRef-Package-Spektrafilm-Android"]
        self.assertEqual("GPL-3.0-only", app["licenseDeclared"])
        self.assertEqual("GPL-3.0-only", app["licenseConcluded"])
        maven_packages = [
            package
            for package in packages.values()
            if package["SPDXID"].startswith("SPDXRef-Maven-")
        ]
        self.assertEqual(3, len(maven_packages))
        purls = {
            package["externalRefs"][0]["referenceLocator"]
            for package in maven_packages
        }
        self.assertEqual(
            {
                "pkg:maven/com.example/alpha@1.0",
                "pkg:maven/com.example/alpha@2.0",
                "pkg:maven/org.demo/ui@3.1",
            },
            purls,
        )
        self.assertTrue(
            all(package["licenseDeclared"] == "NOASSERTION" for package in maven_packages)
        )

    def test_missing_or_extra_lockfile_is_rejected(self) -> None:
        missing = self.repo / "lib/pngwriter/gradle.lockfile"
        original = missing.read_bytes()
        missing.unlink()
        result, output = self._run("missing.spdx.json")
        self._assert_failed(result, output)
        self.assertIn("lockfile inventory", result.stderr)

        missing.write_bytes(original)
        self._write("feature/new-module/gradle.lockfile", b"empty=releaseRuntimeClasspath\n")
        result, output = self._run("extra.spdx.json")
        self._assert_failed(result, output)
        self.assertIn("feature/new-module/gradle.lockfile", result.stderr)

    def test_malformed_lock_entries_fail_but_duplicate_exact_gav_scopes_merge(self) -> None:
        app_lock = self.repo / "app/gradle.lockfile"
        app_lock.write_text(
            app_lock.read_text(encoding="utf-8")
            + "com.example:alpha:1.0=debugRuntimeClasspath\n",
            encoding="utf-8",
        )
        result, output = self._run("merged.spdx.json")
        self.assertEqual(0, result.returncode, result.stderr)
        document = json.loads(output.read_text(encoding="utf-8"))
        alpha_one = [
            package
            for package in document["packages"]
            if package.get("externalRefs", [{}])[0].get("referenceLocator")
            == "pkg:maven/com.example/alpha@1.0"
        ]
        self.assertEqual(1, len(alpha_one))
        self.assertIn("debugRuntimeClasspath", alpha_one[0]["comment"])
        self.assertIn("releaseCompileClasspath", alpha_one[0]["comment"])

        app_lock.write_text("not-a-maven-coordinate=releaseRuntimeClasspath\n", encoding="utf-8")
        result, bad_output = self._run("malformed.spdx.json")
        self._assert_failed(result, bad_output)
        self.assertIn("malformed Maven coordinate", result.stderr)

        # Multiple versions are allowed across isolated configurations, but one
        # module/configuration scope cannot resolve the same GA twice.
        app_lock.write_text(
            "com.example:alpha:1.0=releaseRuntimeClasspath\n"
            "com.example:alpha:2.0=releaseRuntimeClasspath\n",
            encoding="utf-8",
        )
        result, conflict_output = self._run("conflict.spdx.json")
        self._assert_failed(result, conflict_output)
        self.assertIn("version conflict for com.example:alpha", result.stderr)

        app_lock.write_text(
            "empty=releaseRuntimeClasspath\n"
            "com.example:alpha:1.0=releaseRuntimeClasspath\n",
            encoding="utf-8",
        )
        result, empty_conflict_output = self._run("empty-conflict.spdx.json")
        self._assert_failed(result, empty_conflict_output)
        self.assertIn("both empty and dependency-bearing", result.stderr)

    def test_libraw_external_reference_and_dependency_relationship_are_exact(self) -> None:
        result, output = self._run("external.spdx.json")
        self.assertEqual(0, result.returncode, result.stderr)
        document = json.loads(output.read_text(encoding="utf-8"))
        expected_checksum = hashlib.sha256(self.libraw_spdx.read_bytes()).hexdigest()
        self.assertEqual(
            [
                {
                    "externalDocumentId": "DocumentRef-LibRaw",
                    "spdxDocument": "https://example.test/spdx/libraw/0.22.2/fixture",
                    "checksum": {
                        "algorithm": "SHA256",
                        "checksumValue": expected_checksum,
                    },
                }
            ],
            document["externalDocumentRefs"],
        )
        self.assertIn(
            {
                "spdxElementId": "SPDXRef-Package-Spektrafilm-Android",
                "relationshipType": "DEPENDS_ON",
                "relatedSpdxElement": (
                    "DocumentRef-LibRaw:SPDXRef-Package-sfraw-wrapper"
                ),
            },
            document["relationships"],
        )

        invalid = json.loads(self.libraw_spdx.read_text(encoding="utf-8"))
        invalid["documentNamespace"] = "relative/not-a-namespace"
        bad_libraw = self._write(
            "build/invalid-libraw.spdx.json",
            (json.dumps(invalid) + "\n").encode("utf-8"),
        )
        result, bad_output = self._run("bad-libraw.spdx.json", libraw_spdx=bad_libraw)
        self._assert_failed(result, bad_output)
        self.assertIn("LibRaw documentNamespace", result.stderr)

        for index, malformed_namespace in enumerate(
            (
                "https://example.test:bad/spdx/libraw/0.22.2/fixture",
                "https://[example.test/spdx/libraw/0.22.2/fixture",
                "https://[::1]x/spdx/libraw/0.22.2/fixture",
                "https://[fe80::1%25{bad}]/spdx/libraw/0.22.2/fixture",
                "https://example.test/spdx/libraw/{invalid}",
            )
        ):
            with self.subTest(document_namespace=malformed_namespace):
                invalid["documentNamespace"] = malformed_namespace
                malformed_libraw = self._write(
                    f"build/malformed-namespace-{index}.spdx.json",
                    (json.dumps(invalid) + "\n").encode("utf-8"),
                )
                result, malformed_output = self._run(
                    f"malformed-namespace-{index}.output.spdx.json",
                    libraw_spdx=malformed_libraw,
                )
                self._assert_failed(result, malformed_output)
                self.assertIn("LibRaw documentNamespace", result.stderr)

        invalid["documentNamespace"] = "https://example.test/spdx/libraw/0.22.2/no-wrapper"
        invalid["packages"] = []
        missing_wrapper = self._write(
            "build/missing-wrapper-libraw.spdx.json",
            (json.dumps(invalid) + "\n").encode("utf-8"),
        )
        result, bad_output = self._run(
            "missing-wrapper.spdx.json", libraw_spdx=missing_wrapper
        )
        self._assert_failed(result, bad_output)
        self.assertIn("SPDXRef-Package-sfraw-wrapper", result.stderr)

        invalid["packages"] = [{"SPDXID": "SPDXRef-Package-sfraw-wrapper"}]
        incomplete_wrapper = self._write(
            "build/incomplete-wrapper-libraw.spdx.json",
            (json.dumps(invalid) + "\n").encode("utf-8"),
        )
        result, bad_output = self._run(
            "incomplete-wrapper.spdx.json", libraw_spdx=incomplete_wrapper
        )
        self._assert_failed(result, bad_output)
        self.assertIn("LibRaw wrapper package", result.stderr)

        invalid = json.loads(self.libraw_spdx.read_text(encoding="utf-8"))
        invalid["files"] = [{"SPDXID": "SPDXRef-File-wrapper-fixture"}]
        malformed_file = self._write(
            "build/malformed-wrapper-file-libraw.spdx.json",
            (json.dumps(invalid) + "\n").encode("utf-8"),
        )
        result, bad_output = self._run(
            "malformed-wrapper-file.spdx.json", libraw_spdx=malformed_file
        )
        self._assert_failed(result, bad_output)
        self.assertIn("LibRaw SPDX file", result.stderr)

        invalid = json.loads(self.libraw_spdx.read_text(encoding="utf-8"))
        wrapper = next(
            package
            for package in invalid["packages"]
            if package["SPDXID"] == "SPDXRef-Package-sfraw-wrapper"
        )
        wrapper["packageVerificationCode"]["packageVerificationCodeValue"] = "0" * 40
        forged_verification = self._write(
            "build/forged-verification-libraw.spdx.json",
            (json.dumps(invalid) + "\n").encode("utf-8"),
        )
        result, bad_output = self._run(
            "forged-verification.spdx.json", libraw_spdx=forged_verification
        )
        self._assert_failed(result, bad_output)
        self.assertIn("packageVerificationCode does not match", result.stderr)

        invalid = json.loads(self.libraw_spdx.read_text(encoding="utf-8"))
        invalid["files"][0]["SPDXID"] = "SPDXRef-Package-sfraw-wrapper"
        wrapper = next(
            package
            for package in invalid["packages"]
            if package["SPDXID"] == "SPDXRef-Package-sfraw-wrapper"
        )
        wrapper["hasFiles"] = ["SPDXRef-Package-sfraw-wrapper"]
        colliding_element = self._write(
            "build/colliding-element-libraw.spdx.json",
            (json.dumps(invalid) + "\n").encode("utf-8"),
        )
        result, bad_output = self._run(
            "colliding-element.spdx.json", libraw_spdx=colliding_element
        )
        self._assert_failed(result, bad_output)
        self.assertIn("document-wide SPDXID collision", result.stderr)

        invalid = json.loads(self.libraw_spdx.read_text(encoding="utf-8"))
        invalid["files"][0]["checksums"].append(
            {"algorithm": "BOGUS", "checksumValue": "a"}
        )
        invalid_checksum = self._write(
            "build/invalid-checksum-libraw.spdx.json",
            (json.dumps(invalid) + "\n").encode("utf-8"),
        )
        result, bad_output = self._run(
            "invalid-checksum.spdx.json", libraw_spdx=invalid_checksum
        )
        self._assert_failed(result, bad_output)
        self.assertIn("unsupported checksum algorithm", result.stderr)

    def test_changed_lockfile_changes_document_and_namespace(self) -> None:
        first_result, first = self._run("before.spdx.json")
        self.assertEqual(0, first_result.returncode, first_result.stderr)
        first_document = json.loads(first.read_text(encoding="utf-8"))

        lockfile = self.repo / "lib/tiffwriter/gradle.lockfile"
        lockfile.write_text("net.example:codec:9.2=releaseRuntimeClasspath\n", encoding="utf-8")
        second_result, second = self._run("after.spdx.json")
        self.assertEqual(0, second_result.returncode, second_result.stderr)
        second_document = json.loads(second.read_text(encoding="utf-8"))

        self.assertNotEqual(first.read_bytes(), second.read_bytes())
        self.assertNotEqual(
            first_document["documentNamespace"],
            second_document["documentNamespace"],
        )
        second_purls = {
            package.get("externalRefs", [{}])[0].get("referenceLocator")
            for package in second_document["packages"]
        }
        self.assertIn("pkg:maven/net.example/codec@9.2", second_purls)

    def test_release_identity_rejects_nonstable_tags_and_non_sha1_object_ids(self) -> None:
        invalid_versions = ("0.9.0", "v01.2.3", "v1.02.3", "v1.2.03", "v1.2.3-rc1")
        for index, invalid_version in enumerate(invalid_versions):
            with self.subTest(version=invalid_version):
                result, output = self._run(
                    f"invalid-version-{index}.spdx.json", version=invalid_version
                )
                self._assert_failed(result, output)
                self.assertIn("stable vMAJOR.MINOR.PATCH", result.stderr)

        invalid_shas = (
            SOURCE_SHA.upper(),
            SOURCE_SHA + "89abcdef0123456789abcdef",
            SOURCE_SHA[:-1],
        )
        for index, invalid_sha in enumerate(invalid_shas):
            with self.subTest(source_sha=invalid_sha):
                result, output = self._run(
                    f"invalid-sha-{index}.spdx.json", source_sha=invalid_sha
                )
                self._assert_failed(result, output)
                self.assertIn("exactly 40 lowercase hex", result.stderr)


if __name__ == "__main__":
    unittest.main()

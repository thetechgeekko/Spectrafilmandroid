"""Mutation tests for exact release-gate receipts."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "tools/release/gate_attestation.py"
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
SHA = "0123456789abcdef0123456789abcdef01234567"


class GateAttestationTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)
        self.markers = self.root / "markers"
        self.markers.mkdir()
        for gate in GATES:
            (self.markers / gate).write_bytes(b"PASS\n")
        self.unsigned = self.root / "app-release-unsigned.apk"
        self.test_apk = self.root / "app-release-androidTest.apk"
        self.engine_test_apk = self.root / "engine-debug-androidTest.apk"
        self.unsigned.write_bytes(b"unsigned-apk")
        self.test_apk.write_bytes(b"test-apk")
        self.engine_test_apk.write_bytes(b"engine-test-apk")
        self.attestation = self.root / "attestation.txt"

    def _run(self, action: str, *extra: str) -> subprocess.CompletedProcess[str]:
        command = [
            sys.executable,
            str(SCRIPT),
            action,
            "--version",
            "v1.2.3",
            "--source-sha",
            SHA,
            "--run-id",
            "42",
            "--run-attempt",
            "2",
            "--unsigned-apk",
            str(self.unsigned),
            "--instrumentation-apk",
            str(self.test_apk),
            "--engine-instrumentation-apk",
            str(self.engine_test_apk),
            "--attestation",
            str(self.attestation),
            *extra,
        ]
        return subprocess.run(command, capture_output=True, text=True, check=False)

    def _create(self) -> subprocess.CompletedProcess[str]:
        return self._run("create", "--marker-dir", str(self.markers))

    def test_create_and_verify_are_deterministic(self) -> None:
        first = self._create()
        self.assertEqual(0, first.returncode, first.stderr)
        payload = self.attestation.read_bytes()
        second = self._create()
        self.assertEqual(0, second.returncode, second.stderr)
        self.assertEqual(payload, self.attestation.read_bytes())
        verified = self._run("verify")
        self.assertEqual(0, verified.returncode, verified.stderr)
        for gate in GATES:
            self.assertIn(f"gate.{gate}=PASS\n".encode(), payload)

    def test_native_safety_qualification_is_part_of_the_fixed_inventory(self) -> None:
        result = self._create()
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn(b"gate.native-safety-sanitizers=PASS\n", self.attestation.read_bytes())

    def test_engine_boundary_apk_and_assembly_gate_are_hash_bound(self) -> None:
        result = self._create()
        self.assertEqual(0, result.returncode, result.stderr)
        payload = self.attestation.read_bytes()
        self.assertIn(b"gate.engine-jni-boundary-assembly=PASS\n", payload)
        self.assertIn(b"engine_instrumentation_apk_sha256=", payload)
        self.engine_test_apk.write_bytes(b"changed-engine-test-apk")
        self.assertEqual(2, self._run("verify").returncode)

    def test_missing_extra_or_failed_marker_is_rejected(self) -> None:
        (self.markers / GATES[0]).unlink()
        self.assertEqual(2, self._create().returncode)
        (self.markers / GATES[0]).write_bytes(b"PASS\n")
        (self.markers / "unexpected").write_bytes(b"PASS\n")
        self.assertEqual(2, self._create().returncode)
        (self.markers / "unexpected").unlink()
        (self.markers / GATES[-1]).write_bytes(b"FAIL\n")
        self.assertEqual(2, self._create().returncode)

    def test_mutation_stale_attempt_and_apk_drift_are_rejected(self) -> None:
        self.assertEqual(0, self._create().returncode)
        self.attestation.write_bytes(self.attestation.read_bytes() + b"extra\n")
        self.assertEqual(2, self._run("verify").returncode)
        self.assertEqual(0, self._create().returncode)
        stale = self._run("verify", "--run-attempt", "3")
        self.assertEqual(2, stale.returncode)
        self.test_apk.write_bytes(b"changed-test-apk")
        self.assertEqual(2, self._run("verify").returncode)

    def test_invalid_release_identity_is_rejected(self) -> None:
        result = self._run("create", "--marker-dir", str(self.markers), "--version", "v01.2.3")
        self.assertEqual(2, result.returncode)
        result = self._run("create", "--marker-dir", str(self.markers), "--source-sha", "A" * 40)
        self.assertEqual(2, result.returncode)


if __name__ == "__main__":
    unittest.main()

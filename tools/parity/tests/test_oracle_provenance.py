"""Offline integration tests for the parity oracle Git provenance gate."""

from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / "gen_goldens.py"
SPEC = importlib.util.spec_from_file_location("parity_gen_goldens", SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load parity generator: {SCRIPT}")
GEN_GOLDENS = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GEN_GOLDENS
SPEC.loader.exec_module(GEN_GOLDENS)


class OracleRepository:
    """A real local Git checkout used without network access."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.package = root / "src" / "spektrafilm"
        self.package.mkdir(parents=True)
        self.module = self.package / "__init__.py"
        self.module.write_text("fixture = True\n", encoding="utf-8")
        (root / ".gitignore").write_text(
            "ignored.txt\n.test-config/\n", encoding="utf-8"
        )

        self._git("init", "--quiet")
        self._git("config", "user.email", "oracle@example.invalid")
        self._git("config", "user.name", "Oracle Fixture")
        self._git("add", ".")
        self._git("commit", "--quiet", "-m", "fixture")
        self.head = self._git("rev-parse", "HEAD").stdout.strip()

    def _git(self, *args: str) -> subprocess.CompletedProcess[str]:
        environment = {
            name: value
            for name, value in os.environ.items()
            if not name.upper().startswith("GIT_")
        }
        environment["GIT_CONFIG_GLOBAL"] = os.devnull
        environment["GIT_CONFIG_NOSYSTEM"] = "1"
        environment["GIT_OPTIONAL_LOCKS"] = "0"
        result = subprocess.run(
            ["git", *args],
            cwd=self.root,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=environment,
            check=False,
        )
        if result.returncode:
            raise AssertionError(
                f"git {' '.join(args)} failed ({result.returncode}): {result.stderr}"
            )
        return result


class OracleProvenanceTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.checkout = OracleRepository(Path(temporary.name))

    def test_clean_checkout_resolves_root_and_records_verified_state(self) -> None:
        provenance = GEN_GOLDENS.verify_oracle_provenance(
            self.checkout.module, self.checkout.head
        )

        self.assertEqual(self.checkout.root.resolve(), provenance.repository_root)
        self.assertEqual(
            {"commit": self.checkout.head, "worktree": "clean"},
            provenance.as_manifest(),
        )

    def test_git_queries_trust_only_the_exact_checkout_root(self) -> None:
        original_run = subprocess.run
        with mock.patch.object(
            GEN_GOLDENS.subprocess, "run", wraps=original_run
        ) as run:
            GEN_GOLDENS.verify_oracle_provenance(
                self.checkout.module, self.checkout.head
            )

        expected = f"safe.directory={self.checkout.root.resolve().as_posix()}"
        commands = [call.args[0] for call in run.call_args_list]
        self.assertTrue(
            commands
            and all(command[:3] == ["git", "-c", expected] for command in commands),
            commands,
        )

    def test_linked_worktree_gitfile_resolves_as_exact_root(self) -> None:
        worktree_parent = tempfile.TemporaryDirectory()
        self.addCleanup(worktree_parent.cleanup)
        worktree = Path(worktree_parent.name) / "linked"
        self.checkout._git(
            "worktree", "add", "--quiet", "--detach", str(worktree), self.checkout.head
        )

        provenance = GEN_GOLDENS.verify_oracle_provenance(
            worktree / "src" / "spektrafilm", self.checkout.head
        )

        self.assertTrue((worktree / ".git").is_file())
        self.assertEqual(worktree.resolve(), provenance.repository_root)

    def test_wrong_head_is_rejected(self) -> None:
        wrong_head = "0" * 40

        with self.assertRaisesRegex(
            GEN_GOLDENS.OracleProvenanceError,
            rf"HEAD mismatch.*expected {wrong_head}.*found {self.checkout.head}",
        ):
            GEN_GOLDENS.verify_oracle_provenance(
                self.checkout.package, wrong_head
            )

    def test_tracked_change_is_rejected(self) -> None:
        self.checkout.module.write_text("fixture = False\n", encoding="utf-8")
        with self.assertRaisesRegex(
            GEN_GOLDENS.OracleProvenanceError,
            r"dirty.*src/spektrafilm/__init__\.py",
        ):
            GEN_GOLDENS.verify_oracle_provenance(
                self.checkout.package, self.checkout.head
            )

    def test_untracked_change_is_rejected(self) -> None:
        untracked = self.checkout.root / "untracked.txt"
        untracked.write_text("not reviewed\n", encoding="utf-8")
        with self.assertRaisesRegex(
            GEN_GOLDENS.OracleProvenanceError,
            r"dirty.*untracked\.txt",
        ):
            GEN_GOLDENS.verify_oracle_provenance(
                self.checkout.package, self.checkout.head
            )

    def test_repository_ignored_file_uses_normal_git_cleanliness(self) -> None:
        ignored = self.checkout.root / "ignored.txt"
        ignored.write_text("local cache\n", encoding="utf-8")

        provenance = GEN_GOLDENS.verify_oracle_provenance(
            self.checkout.package, self.checkout.head
        )

        self.assertEqual("clean", provenance.worktree)

    def test_ambient_global_exclude_cannot_hide_untracked_file(self) -> None:
        excluded = self.checkout.root / "externally-hidden.txt"
        excluded.write_text("must be detected\n", encoding="utf-8")
        config_dir = self.checkout.root / ".test-config"
        config_dir.mkdir()
        excludes_file = config_dir / "external-excludes"
        excludes_file.write_text("externally-hidden.txt\n", encoding="utf-8")
        global_config = config_dir / "external-gitconfig"
        global_config.write_text(
            f"[core]\n\texcludesFile = {excludes_file.as_posix()}\n",
            encoding="utf-8",
        )

        with mock.patch.dict(
            os.environ, {"GIT_CONFIG_GLOBAL": str(global_config)}, clear=False
        ), self.assertRaisesRegex(
            GEN_GOLDENS.OracleProvenanceError,
            r"dirty.*externally-hidden\.txt",
        ):
            GEN_GOLDENS.verify_oracle_provenance(
                self.checkout.package, self.checkout.head
            )

    def test_clean_publish_replaces_artifact_then_manifest(self) -> None:
        provenance = GEN_GOLDENS.verify_oracle_provenance(
            self.checkout.package, self.checkout.head
        )
        publication = tempfile.TemporaryDirectory()
        self.addCleanup(publication.cleanup)
        root = Path(publication.name)
        staged_artifact = root / "staged.bin"
        staged_manifest = root / "staged-manifest.json"
        destination = root / "published.bin"
        manifest_destination = root / "manifest.json"
        staged_artifact.write_bytes(b"new fixture")
        staged_manifest.write_bytes(b'{"state":"new"}\n')
        destination.write_bytes(b"old fixture")
        manifest_destination.write_bytes(b'{"state":"old"}\n')

        final = GEN_GOLDENS.publish_verified_artifacts(
            [(staged_artifact, destination)],
            staged_manifest,
            manifest_destination,
            provenance,
        )

        self.assertEqual(b"new fixture", destination.read_bytes())
        self.assertEqual(b'{"state":"new"}\n', manifest_destination.read_bytes())
        self.assertEqual(self.checkout.head, final.commit)

    def test_late_dirty_checkout_removes_old_manifest(self) -> None:
        provenance = GEN_GOLDENS.verify_oracle_provenance(
            self.checkout.package, self.checkout.head
        )
        publication = tempfile.TemporaryDirectory()
        self.addCleanup(publication.cleanup)
        root = Path(publication.name)
        staged_artifact = root / "staged.bin"
        staged_manifest = root / "staged-manifest.json"
        destination = root / "published.bin"
        manifest_destination = root / "manifest.json"
        staged_artifact.write_bytes(b"new fixture")
        staged_manifest.write_bytes(b'{"state":"new"}\n')
        destination.write_bytes(b"old fixture")
        manifest_destination.write_bytes(b'{"state":"old"}\n')
        (self.checkout.root / "runtime-change.txt").write_text(
            "oracle mutated\n", encoding="utf-8"
        )

        with self.assertRaisesRegex(
            GEN_GOLDENS.OracleProvenanceError, r"dirty.*runtime-change\.txt"
        ):
            GEN_GOLDENS.publish_verified_artifacts(
                [(staged_artifact, destination)],
                staged_manifest,
                manifest_destination,
                provenance,
            )

        self.assertEqual(b"new fixture", destination.read_bytes())
        self.assertFalse(manifest_destination.exists())


if __name__ == "__main__":
    unittest.main()

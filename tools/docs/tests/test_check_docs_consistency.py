from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "check_docs_consistency.py"
SPEC = importlib.util.spec_from_file_location("check_docs_consistency", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class DocsConsistencyTest(unittest.TestCase):
    def test_extract_returns_first_capture_and_rejects_missing(self) -> None:
        source = checker.ROOT / "sample.txt"
        self.assertEqual(checker._extract(r"value=(\d+)", "value=39", source), "39")
        with self.assertRaises(ValueError):
            checker._extract(r"missing=(\d+)", "value=39", source)

    def test_local_link_checker_handles_present_missing_external_and_escape(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            docs = root / "docs"
            docs.mkdir()
            page = docs / "page.md"
            (docs / "exists.md").write_text("ok", encoding="utf-8")
            text = "[ok](exists.md) [missing](no.md) [web](https://example.com) [escape](../../x)"
            with mock.patch.object(checker, "ROOT", root):
                errors = checker._check_local_links(page, text)
        self.assertEqual(len(errors), 2)
        self.assertTrue(any("missing local link" in error for error in errors))
        self.assertTrue(any("escapes repository" in error for error in errors))

    def test_local_link_checker_accepts_balanced_parentheses_and_angle_spaces(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            docs = root / "docs"
            docs.mkdir()
            page = docs / "page.md"
            (docs / "file_(v2).md").write_text("ok", encoding="utf-8")
            spaced = docs / "folder with spaces"
            spaced.mkdir()
            (spaced / "file.md").write_text("ok", encoding="utf-8")
            text = (
                "[balanced](file_(v2).md) "
                "[spaced](<folder with spaces/file.md> \"optional title\")"
            )
            with mock.patch.object(checker, "ROOT", root):
                errors = checker._check_local_links(page, text)
        self.assertEqual(errors, [])

    def test_commonmark_destination_escapes_are_unescaped_before_classification(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            docs = root / "docs"
            nested = docs / "nested"
            nested.mkdir(parents=True)
            page = docs / "page.md"
            (nested / "file_(v2).md").write_text("ok", encoding="utf-8")
            text = (
                r"[parentheses](nested/file_\(v2\).md) "
                r"[backslash](nested\\file_\(v2\).md) "
                r"[external](HTTPS\://example.invalid/path)"
            )
            with mock.patch.object(checker, "ROOT", root):
                errors = checker._check_local_links(page, text)
        self.assertEqual(errors, [])

    def test_local_link_checker_enforces_exact_nested_component_spelling(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            docs = root / "docs"
            nested = docs / "Parent" / "Child"
            nested.mkdir(parents=True)
            page = docs / "page.md"
            (nested / "Target.md").write_text("ok", encoding="utf-8")
            text = (
                "[exact](Parent/Child/Target.md) "
                "[parent](parent/Child/Target.md) "
                "[child](Parent/child/Target.md) "
                "[file](Parent/Child/target.md)"
            )
            with mock.patch.object(checker, "ROOT", root):
                errors = checker._check_local_links(page, text)
        self.assertEqual(len(errors), 3)
        self.assertTrue(all("on-disk spelling mismatch" in error for error in errors))

    def test_local_link_checker_rejects_trailing_dot_or_space_in_any_component(self) -> None:
        page = checker.ROOT / "docs" / "page.md"
        text = (
            "[dot](existing.md.) "
            "[space](<existing.md >) "
            "[nested-dot](folder./file.md) "
            "[nested-space](folder%20/file.md)"
        )
        with mock.patch.object(Path, "resolve", side_effect=AssertionError("must not resolve")):
            errors = checker._check_local_links(page, text)
        self.assertEqual(len(errors), 4)
        self.assertTrue(all("trailing dot or space" in error for error in errors))

    def test_local_link_checker_rejects_percent_decoded_control_characters(self) -> None:
        page = checker.ROOT / "docs" / "page.md"
        text = "[nul](exists%00.md) [tab](exists%09.md) [del](exists%7F.md)"
        with mock.patch.object(Path, "resolve", side_effect=AssertionError("must not resolve")):
            errors = checker._check_local_links(page, text)
        self.assertEqual(len(errors), 3)
        self.assertTrue(all("control character" in error for error in errors))

    def test_reference_links_and_images_are_checked_but_fenced_examples_are_not(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            docs = root / "docs"
            docs.mkdir()
            page = docs / "page.md"
            (docs / "exists.png").write_bytes(b"image")
            text = (
                "![image](exists.png) [reference][ok]\n"
                "[ok]: exists.png\n"
                "```markdown\n[example](missing-example.md)\n```\n"
            )
            with mock.patch.object(checker, "ROOT", root):
                errors = checker._check_local_links(page, text)
        self.assertEqual(errors, [])

    def test_external_and_scheme_relative_links_never_resolve_as_paths(self) -> None:
        page = checker.ROOT / "docs" / "page.md"
        text = (
            "[upper](HTTPS://example.invalid/path) "
            "[network](//example.invalid/share) [mail](MAILTO:test@example.invalid)"
        )
        with mock.patch.object(Path, "resolve", side_effect=AssertionError("must not resolve")):
            self.assertEqual(checker._check_local_links(page, text), [])

    def test_file_scheme_is_rejected_without_path_resolution(self) -> None:
        page = checker.ROOT / "docs" / "page.md"
        with mock.patch.object(Path, "resolve", side_effect=AssertionError("must not resolve")):
            errors = checker._check_local_links(page, "[local](file:///etc/passwd)")
        self.assertEqual(len(errors), 1)
        self.assertIn("unsupported link scheme 'file'", errors[0])

    def test_unsupported_authority_schemes_are_rejected_after_unescaping(self) -> None:
        page = checker.ROOT / "docs" / "page.md"
        text = r"[file](FILE\://server/share) [ssh](SSH\://host/path)"
        with mock.patch.object(Path, "resolve", side_effect=AssertionError("must not resolve")):
            errors = checker._check_local_links(page, text)
        self.assertEqual(len(errors), 2)
        self.assertIn("unsupported link scheme 'file'", errors[0])
        self.assertIn("unsupported link scheme 'ssh'", errors[1])

    def test_percent_encoded_network_paths_never_reach_pathlib(self) -> None:
        page = checker.ROOT / "docs" / "page.md"
        text = "[forward](%2F%2Fevil.invalid/share) [back](%5C%5Cevil%5Cshare)"
        with mock.patch.object(Path, "resolve", side_effect=AssertionError("must not resolve")):
            errors = checker._check_local_links(page, text)
        self.assertEqual(len(errors), 2)
        self.assertTrue(all("link target" in error for error in errors))

    def test_preset_set_comparison_detects_missing_extra_and_duplicates(self) -> None:
        errors = checker._preset_set_errors(["a", "a", "b"], ["a", "c", "c"])
        joined = "\n".join(errors)
        self.assertIn("duplicate preset ID", joined)
        self.assertIn("duplicate documented preset ID", joined)
        self.assertIn("missing preset IDs: b", joined)
        self.assertIn("unknown preset IDs: c", joined)

    def test_preset_set_comparison_accepts_equal_sets_in_different_order(self) -> None:
        self.assertEqual(checker._preset_set_errors(["a", "b"], ["b", "a"]), [])

    def test_sdk_summary_keeps_target_and_compile_independent(self) -> None:
        self.assertEqual(
            checker._sdk_summary("24", "34", "35"),
            "min 24, target 34, compile 35",
        )

    def test_included_gradles_ignore_dormant_decoy_modules(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            included = root / "app" / "build.gradle.kts"
            decoy = root / "dormant" / "build.gradle.kts"
            included.parent.mkdir()
            decoy.parent.mkdir()
            included.write_text("android {}", encoding="utf-8")
            decoy.write_text("externalNativeBuild { stale() }", encoding="utf-8")
            with mock.patch.object(checker, "ROOT", root):
                paths = checker._included_module_gradles('include(":app")')
        self.assertEqual(paths, [included])

    def test_workflow_pins_detect_drift(self) -> None:
        workflow = checker.ROOT / ".github" / "workflows" / "sample.yml"
        errors = checker._workflow_pin_errors(
            {
                workflow: (
                    'sdkmanager "ndk;27.0" "cmake;3.22.1" '
                    '"build-tools;36.0.0"'
                )
            },
            {"ndk": "27.0", "cmake": "3.22.1", "build-tools": "35.0.0"},
        )
        self.assertEqual(len(errors), 1)
        self.assertIn("expected build-tools 35.0.0, found 36.0.0", errors[0])

    def test_parity_flags_detect_ci_and_release_drift(self) -> None:
        shipping = "-O3 -ffast-math -fno-finite-math-only"
        good_ci = f'          opt: "-O2"\n          opt: "{shipping}"\n'
        good_release = f"          SPK_PARITY_EXTRA_FLAGS: {shipping}\n"
        self.assertEqual(checker._parity_flag_errors(good_ci, good_release, shipping), [])
        errors = checker._parity_flag_errors(
            '          opt: "-O2"\n',
            "          SPK_PARITY_EXTRA_FLAGS: -O3\n",
            shipping,
        )
        self.assertEqual(len(errors), 2)


if __name__ == "__main__":
    unittest.main()

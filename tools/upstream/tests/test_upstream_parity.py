"""Tests for the #189 upstream parity manifest tooling."""
from __future__ import annotations

import copy
import json
import pathlib
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools" / "upstream"))

import gen_upstream_parity as gup  # noqa: E402

PIN = json.loads((REPO / "tools" / "upstream" / "upstream_pin.json").read_text(encoding="utf-8"))
MANIFEST = json.loads(
    (REPO / "tools" / "upstream" / "parity_manifest.json").read_text(encoding="utf-8"))


class PinTest(unittest.TestCase):
    def test_oracle_matches_the_golden_provenance_pin(self):
        # tools/parity/setup_env.sh is the long-standing authority for the oracle SHA.
        env = (REPO / "tools" / "parity" / "setup_env.sh").read_text(encoding="utf-8")
        self.assertIn(PIN["oracle"]["sha"], env)

    def test_reviewed_and_oracle_are_full_shas(self):
        for sha in (PIN["oracle"]["sha"], PIN["reviewed"]["sha"], PIN["reviewed"]["tree"]):
            self.assertEqual(40, len(sha))
            int(sha, 16)

    def test_asset_tree_digest_matches_the_bundle(self):
        digest, files = gup.asset_tree_sha256(REPO / PIN["assets"]["path"])
        self.assertEqual(PIN["assets"]["tree_sha256"], digest)
        self.assertEqual(PIN["assets"]["files"], files)


class ManifestTest(unittest.TestCase):
    def test_committed_state_validates(self):
        self.assertEqual([], gup.validate(PIN, MANIFEST))

    def test_committed_doc_is_generated(self):
        expected = gup.render(PIN, MANIFEST)
        actual = (REPO / "docs" / "UPSTREAM_PARITY.md").read_text(encoding="utf-8")
        self.assertEqual(expected, actual)

    def test_every_missing_item_links_a_ticket(self):
        for item in MANIFEST["items"]:
            if item["status"] == "missing":
                self.assertIn("github.com/thetechgeekko/Spektrafilm-android/issues/",
                              item["ticket"], item["id"])

    def test_every_inapplicable_item_has_a_rationale(self):
        for item in MANIFEST["items"]:
            if item["status"] == "inapplicable":
                self.assertTrue(item.get("rationale"), item["id"])

    def test_missing_without_ticket_is_rejected(self):
        manifest = copy.deepcopy(MANIFEST)
        for item in manifest["items"]:
            if item["status"] == "missing":
                item.pop("ticket", None)
        problems = gup.validate(PIN, manifest)
        self.assertTrue(any("without an atomic ticket" in p for p in problems))

    def test_double_status_is_impossible_by_construction(self):
        # One string field, validated against the closed vocabulary: an item cannot
        # carry two statuses. An unknown status is rejected.
        manifest = copy.deepcopy(MANIFEST)
        manifest["items"][0]["status"] = "matched-and-missing"
        problems = gup.validate(PIN, manifest)
        self.assertTrue(any("unknown status" in p for p in problems))

    def test_duplicate_id_is_rejected(self):
        manifest = copy.deepcopy(MANIFEST)
        manifest["items"].append(dict(manifest["items"][0]))
        problems = gup.validate(PIN, manifest)
        self.assertTrue(any("duplicate item id" in p for p in problems))

    def test_manifest_shas_must_agree_with_the_pin(self):
        manifest = copy.deepcopy(MANIFEST)
        manifest["reviewed_sha"] = "0" * 40
        problems = gup.validate(PIN, manifest)
        self.assertTrue(any("reviewed_sha disagrees" in p for p in problems))

    def test_areas_are_covered(self):
        areas = {item["area"] for item in MANIFEST["items"]}
        # The #189 acceptance list: stages/taps, params/defaults, profiles/assets,
        # GUI behavior, LUT formats, Android extensions.
        for required in ("stages", "parameters", "profiles-assets", "gui",
                         "lut-formats", "android-extensions"):
            self.assertIn(required, areas)


if __name__ == "__main__":
    unittest.main()

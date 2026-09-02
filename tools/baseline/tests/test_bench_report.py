"""Tests for the #177 benchmark reporter and the pinned corpus."""
from __future__ import annotations

import copy
import json
import pathlib
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools" / "baseline"))
sys.path.insert(0, str(REPO / "tools" / "baseline" / "corpus"))

import bench_report  # noqa: E402
import make_source  # noqa: E402

CORPUS = json.loads((REPO / "tools" / "baseline" / "corpus.json").read_text(encoding="utf-8"))
FIXTURE = pathlib.Path(__file__).with_name("fixtures") / "capture-gate.json"


def load_fixture() -> dict:
    return json.loads(FIXTURE.read_text(encoding="utf-8"))


def write(capture: dict) -> pathlib.Path:
    handle = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8")
    with handle as sink:
        json.dump(capture, sink)
    return pathlib.Path(handle.name)


def run(capture: dict, *extra: str) -> int:
    path = write(capture)
    try:
        return bench_report.main([str(path), "--corpus",
                                  str(REPO / "tools" / "baseline" / "corpus.json"), *extra])
    finally:
        path.unlink(missing_ok=True)


class CorpusTest(unittest.TestCase):
    def test_source_generator_matches_the_pin(self):
        self.assertEqual(0, make_source.main(["--check"]))

    def test_source_bytes_are_reproducible(self):
        spec = CORPUS["source"]
        first = make_source.png_bytes(64, 48)
        second = make_source.png_bytes(64, 48)
        self.assertEqual(first, second)
        self.assertGreater(spec["bytes"], 0)
        self.assertEqual(64, len(spec["sha256"]))

    def test_every_cell_declares_gates_and_formats(self):
        ids = set()
        for cell in CORPUS["cells"]:
            self.assertNotIn(cell["id"], ids, "duplicate cell id")
            ids.add(cell["id"])
            self.assertTrue(cell["gates"], f"{cell['id']} gates nothing")
            for fmt in cell["formats"]:
                self.assertIn(fmt, CORPUS["container_identity"],
                              f"{fmt} has no container-identity policy")
        # BASE/HEAVY carry the #126 gates; the grain/route cells are the #119 matrix.
        self.assertEqual(
            {"BASE", "HEAVY", "PRINT_GRAIN", "SCAN_CLEAN", "SCAN_GRAIN"}, ids)

    def test_jpeg_container_identity_is_unsupported(self):
        # #126: a JPEG's container bytes are an encoder artifact, not a contract.
        self.assertEqual("unsupported", CORPUS["container_identity"]["JPEG_Q95"])


class StatisticsTest(unittest.TestCase):
    def test_percentile_is_nearest_rank(self):
        values = [10.0, 20.0, 30.0, 40.0]
        self.assertEqual(20.0, bench_report.percentile(values, 50))
        self.assertEqual(40.0, bench_report.percentile(values, 95))

    def test_percentile_rejects_empty_input(self):
        with self.assertRaises(bench_report.ReportError):
            bench_report.percentile([], 50)

    def test_confidence_interval_needs_two_samples(self):
        mean, half = bench_report.mean_ci95([5.0])
        self.assertEqual(5.0, mean)
        self.assertNotEqual(half, half)  # NaN


class GateTest(unittest.TestCase):
    def test_clean_capture_passes_the_gate(self):
        self.assertEqual(0, run(load_fixture(), "--gate"))

    def test_smoke_capture_is_refused_as_a_baseline(self):
        capture = load_fixture()
        capture["protocol"]["smoke"] = True
        capture["protocol"]["requested_runs"] = 2
        self.assertEqual(2, run(capture, "--gate"))

    def test_smoke_capture_still_reports_without_the_gate(self):
        capture = load_fixture()
        capture["protocol"]["smoke"] = True
        self.assertEqual(0, run(capture))

    def test_stale_apk_fails_closed(self):
        self.assertEqual(2, run(load_fixture(), "--expect-app-sha256", "ab" * 32))

    def test_debuggable_capture_is_refused(self):
        capture = load_fixture()
        capture["app"]["debuggable"] = True
        self.assertEqual(2, run(capture))

    def test_corpus_drift_fails_closed(self):
        capture = load_fixture()
        capture["corpus"]["source_sha256"] = "00" * 32
        self.assertEqual(2, run(capture))

    def test_engine_digest_divergence_breaks_c0(self):
        capture = load_fixture()
        capture["samples"][1]["engine_sample_sha256"] = "11" * 32
        self.assertEqual(1, run(capture))

    def test_decoded_digest_divergence_breaks_c3(self):
        capture = load_fixture()
        capture["samples"][1]["decoded_sample_sha256"] = "22" * 32
        self.assertEqual(1, run(capture))

    def test_container_divergence_is_ignored_where_c4_is_unsupported(self):
        capture = load_fixture()
        for sample in capture["samples"]:
            if sample["format"] == "JPEG_Q95":
                sample["container_sha256"] = f"{sample['run_index']:064d}"
        self.assertEqual(0, run(capture, "--gate"))

    def test_container_divergence_breaks_gated_c4(self):
        capture = load_fixture()
        for sample in capture["samples"]:
            if sample["format"] == "PNG16":
                sample["container_sha256"] = f"{sample['run_index']:064d}"
        self.assertEqual(1, run(capture))

    def test_unaccounted_stage_time_is_reported(self):
        capture = load_fixture()
        capture["samples"][0]["total_ms"] = capture["samples"][0]["phases_sum_ms"] + 5_000
        self.assertEqual(1, run(capture))

    def test_slo_breach_fails_only_under_the_gate(self):
        capture = load_fixture()
        for sample in capture["samples"]:
            if sample["cell"] == "BASE" and sample["format"] == "JPEG_Q95":
                sample["total_ms"] = 9_000
                sample["phases_sum_ms"] = 9_000
                sample["phases_ms"] = {"decode": 1_000, "simulate": 7_000,
                                       "grade": 500, "encode": 500}
        self.assertEqual(0, run(capture))
        self.assertEqual(1, run(capture, "--gate"))

    def test_full_rerender_only_capture_cannot_claim_the_slo(self):
        capture = load_fixture()
        for sample in capture["samples"]:
            sample["served_from_cache"] = False
        self.assertEqual(0, run(capture))
        self.assertEqual(1, run(capture, "--gate"))

    def test_wrong_device_fails_the_gate_only(self):
        capture = load_fixture()
        capture["device"]["model"] = "Pixel 9"
        self.assertEqual(0, run(capture))
        self.assertEqual(1, run(capture, "--gate"))

    def test_plugged_in_run_fails_the_gate(self):
        capture = load_fixture()
        capture["samples"][0]["environment"]["plugged"] = 2
        self.assertEqual(1, run(capture, "--gate"))

    def test_throttled_run_fails_the_gate(self):
        capture = load_fixture()
        capture["samples"][0]["environment"]["thermal_status"] = 3
        self.assertEqual(1, run(capture, "--gate"))

    def test_failed_journey_is_reported(self):
        capture = load_fixture()
        capture["journeys"]["reopen_published"] = "fail:reopen=false share=true"
        self.assertEqual(1, run(capture))

    def test_background_export_is_reported(self):
        capture = load_fixture()
        capture["journeys"]["foreground_during_export"] = False
        self.assertEqual(1, run(capture))

    def test_missing_sample_field_is_refused(self):
        capture = load_fixture()
        del capture["samples"][0]["container_sha256"]
        self.assertEqual(2, run(capture))

    def test_unknown_schema_is_refused(self):
        capture = load_fixture()
        capture["schema"] = "spk.bench_capture.v0"
        self.assertEqual(2, run(capture))

    def test_report_names_the_capture_kind(self):
        capture = load_fixture()
        report = bench_report.render(capture, CORPUS)
        self.assertIn("BASELINE", report)
        smoke = copy.deepcopy(capture)
        smoke["protocol"]["smoke"] = True
        self.assertIn("SMOKE", bench_report.render(smoke, CORPUS))


if __name__ == "__main__":
    unittest.main()

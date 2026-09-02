"""Tests for the #119 preview-latency reporter."""
from __future__ import annotations

import copy
import pathlib
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools" / "baseline"))

import preview_report  # noqa: E402


def capture() -> dict:
    return {
        "schema": "spk.bench_preview.v1",
        "app": {"apk_sha256": "a" * 64, "version_name": "0.9.0", "version_code": 11},
        "device": {"model": "SM-S948W", "sdk_int": 36, "build_fingerprint": "fp"},
        "environment": {"thermal_status": 0, "battery_pct": 78, "plugged": 0,
                        "cpuset": "/"},
        "source_sha256": "b" * 64,
        "decode_max_edge": 4096,
        "routes": [
            {"route": "print", "preset": "P", "preview_max_size": 640,
             "samples_ms": [100, 102, 98, 101, 99, 100, 103, 97, 100, 101, 100, 99]},
            {"route": "scan", "preset": "P", "preview_max_size": 640,
             "samples_ms": [80, 82, 78, 81, 79, 80, 83, 77, 80, 81, 80, 79]},
        ],
    }


class RenderTests(unittest.TestCase):
    def test_renders_both_routes_with_stats(self) -> None:
        text = preview_report.render(capture())
        self.assertIn("| print | 12 | 100 |", text)
        self.assertIn("| scan | 12 | 80 |", text)
        self.assertIn("ARGB bitmap the editor draws", text)

    def test_engine_and_bitmap_split_is_reported_when_present(self) -> None:
        split = capture()
        split["routes"][0]["engine_ms"] = [90] * 12
        split["routes"][0]["bitmap_ms"] = [10] * 12
        row = [r for r in preview_report.route_rows(split) if r.startswith("| print")][0]
        self.assertTrue(row.rstrip().endswith("| 90 | 10 |"), row)

    def test_missing_split_renders_dashes_rather_than_failing(self) -> None:
        row = [r for r in preview_report.route_rows(capture()) if r.startswith("| scan")][0]
        self.assertTrue(row.rstrip().endswith("| - | - |"), row)

    def test_unplugged_full_capture_passes_the_gate(self) -> None:
        self.assertEqual([], preview_report.gate_findings(capture()))


class GateTests(unittest.TestCase):
    def test_plugged_capture_cannot_claim_the_baseline(self) -> None:
        plugged = copy.deepcopy(capture())
        plugged["environment"]["plugged"] = 2
        findings = preview_report.gate_findings(plugged)
        self.assertTrue(any("plugged" in f for f in findings), findings)

    def test_undersampled_route_is_refused(self) -> None:
        small = copy.deepcopy(capture())
        small["routes"][1]["samples_ms"] = [80, 81, 79]
        findings = preview_report.gate_findings(small)
        self.assertTrue(any("below the 10 minimum" in f for f in findings), findings)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Render the #119 preview-latency capture (spk.bench_preview.v1) as markdown.

Slider-drag settle here means the engine's simulatePreview at the default 640 px
on an already-decoded source — exactly the work the editor repeats while a slider
drags. Compose/present cost is additive UI overhead outside this number, and the
rendered table says so, so nobody can mistake the engine settle for end-to-end
frame latency.

With --gate the capture is held to the #119 protocol: unplugged, and at least 10
samples per route. A capture that fails those is reported anyway (the numbers
stay visible) but the exit code refuses to bless it as a baseline.

Film modeling powered by spektrafilm (GPLv3).
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys

from bench_report import mean_ci95, percentile

SCHEMA = "spk.bench_preview.v1"


def route_rows(capture: dict) -> list[str]:
    rows = []
    for route in capture.get("routes", []):
        ms = [float(v) for v in route.get("samples_ms", [])]
        if not ms:
            rows.append(f"| {route.get('route', '?')} | 0 | - | - | - | - | - |")
            continue
        mean, ci = mean_ci95(ms)

        def med(key: str) -> str:
            values = [float(v) for v in route.get(key, [])]
            return f"{percentile(values, 50):.0f}" if values else "-"

        rows.append(
            f"| {route.get('route', '?')} | {len(ms)} | {percentile(ms, 50):.0f} | "
            f"{percentile(ms, 95):.0f} | {mean:.0f} +/- {ci:.0f} | "
            f"{med('engine_ms')} | {med('bitmap_ms')} |")
    return rows


def render(capture: dict) -> str:
    app = capture.get("app", {})
    device = capture.get("device", {})
    env = capture.get("environment", {})
    lines = [
        "# Preview-latency capture (#119)",
        "",
        f"- app `{str(app.get('apk_sha256', '?'))[:16]}...` "
        f"v{app.get('version_name', '?')} ({app.get('version_code', '?')})",
        f"- device {device.get('model', '?')} sdk {device.get('sdk_int', '?')} "
        f"`{device.get('build_fingerprint', '?')}`",
        f"- thermal {env.get('thermal_status', '?')}, battery {env.get('battery_pct', '?')}%, "
        f"plugged {env.get('plugged', '?')}, cpuset `{env.get('cpuset', '?')}`",
        f"- decode edge {capture.get('decode_max_edge', '?')} px, preview 640 px, "
        f"source `{str(capture.get('source_sha256', '?'))[:16]}...`",
        "",
        "| route | n | p50 ms | p95 ms | mean +/- 95% CI | engine p50 | bitmap p50 |",
        "|---|---|---|---|---|---|---|",
        *route_rows(capture),
        "",
        "Settle = `SpektraEngine.simulatePreview` on an already-decoded source plus the",
        "ARGB bitmap the editor draws, which is the pair a slider drag repeats; the",
        "grade uses the preset's own values, so a neutral preset skips it. Compose",
        "layout/present cost is additive UI overhead outside this number.",
        "",
    ]
    return "\n".join(lines)


def gate_findings(capture: dict) -> list[str]:
    findings = []
    env = capture.get("environment", {})
    if env.get("plugged", -1) != 0:
        findings.append(f"gate capture ran plugged in (plugged={env.get('plugged')!r})")
    routes = capture.get("routes", [])
    if not routes:
        findings.append("capture has no routes")
    for route in routes:
        n = len(route.get("samples_ms", []))
        if n < 10:
            findings.append(f"route {route.get('route', '?')}: {n} samples is below the 10 minimum")
    return findings


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", help="preview.json pulled from the device")
    parser.add_argument("--markdown", help="write the rendered table here")
    parser.add_argument("--gate", action="store_true",
                        help="refuse plugged or under-sampled captures")
    args = parser.parse_args(argv)

    capture = json.loads(pathlib.Path(args.capture).read_text(encoding="utf-8"))
    if capture.get("schema") != SCHEMA:
        print(f"preview_report: unexpected schema {capture.get('schema')!r}", file=sys.stderr)
        return 1

    text = render(capture)
    if args.markdown:
        pathlib.Path(args.markdown).write_text(text, encoding="utf-8", newline="\n")
    print(text)

    findings = gate_findings(capture) if args.gate else []
    for finding in findings:
        print(f"preview_report: {finding}", file=sys.stderr)
    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

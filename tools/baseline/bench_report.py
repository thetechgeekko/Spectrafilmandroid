#!/usr/bin/env python3
"""Validate and report a #177 benchmark capture.

The device harness only measures; every statistic and every pass/fail decision lives
here, so a bounded smoke run can never be published as a baseline.

    python tools/baseline/bench_report.py capture.json                 # report
    python tools/baseline/bench_report.py capture.json --gate          # enforce the contract
    python tools/baseline/bench_report.py capture.json --expect-app-sha256 <hex>

Contract: docs/BIT_IDENTICAL_EXPORT_ROADMAP.md#the-approved-export-contract-issue-126

Film modeling powered by spektrafilm (GPLv3).
"""
from __future__ import annotations

import argparse
import json
import math
import pathlib
import statistics
import sys

CAPTURE_SCHEMA = "spk.bench_capture.v1"
CORPUS_SCHEMA = "spk.bench_corpus.v1"
REPO = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_CORPUS = REPO / "tools" / "baseline" / "corpus.json"

# Identity levels this reporter can decide from a capture alone.
C0 = "C0"  # same build, same device: repeat runs agree byte for byte
C3 = "C3"  # decoded samples + normalized metadata agree
C4 = "C4"  # complete container agrees (only where the corpus says it is gated)

SAMPLE_FIELDS = (
    "cell", "format", "run_index", "state", "total_ms", "phases_ms", "phases_sum_ms",
    "engine_sample_sha256", "decoded_sample_sha256", "normalized_metadata_sha256",
    "container_sha256", "container_bytes", "memory", "environment", "served_from_cache",
)


class ReportError(Exception):
    """A capture that cannot be trusted at all (schema, identity, corpus drift)."""


def percentile(values: list[float], q: float) -> float:
    """Nearest-rank percentile: no interpolation invents a sample we did not measure."""
    if not values:
        raise ReportError("percentile of an empty sample set")
    ordered = sorted(values)
    rank = max(1, math.ceil(q / 100.0 * len(ordered)))
    return ordered[min(rank, len(ordered)) - 1]


def mean_ci95(values: list[float]) -> tuple[float, float]:
    """Mean and half-width of a normal-approximation 95% confidence interval."""
    if len(values) < 2:
        return (values[0] if values else 0.0), float("nan")
    mean = statistics.fmean(values)
    half = 1.96 * statistics.stdev(values) / math.sqrt(len(values))
    return mean, half


def load(path: pathlib.Path) -> dict:
    try:
        capture = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as failure:
        raise ReportError(f"unreadable capture {path}: {failure}") from failure
    if capture.get("schema") != CAPTURE_SCHEMA:
        raise ReportError(f"unexpected capture schema {capture.get('schema')!r}")
    for section in ("app", "device", "corpus", "protocol", "samples"):
        if section not in capture:
            raise ReportError(f"capture is missing the {section!r} section")
    if not capture["samples"]:
        raise ReportError("capture contains no samples")
    for index, sample in enumerate(capture["samples"]):
        missing = [field for field in SAMPLE_FIELDS if field not in sample]
        if missing:
            raise ReportError(f"sample {index} is missing {', '.join(missing)}")
    return capture


def check_artifact(capture: dict, corpus: dict, expect_app_sha256: str | None) -> list[str]:
    """Fail closed on a stale APK or a drifted corpus before any number is reported."""
    problems: list[str] = []
    app = capture["app"]
    if app.get("debuggable"):
        problems.append("capture came from a debuggable build")
    if expect_app_sha256:
        actual = str(app.get("apk_sha256", "")).lower()
        if actual != expect_app_sha256.lower():
            problems.append(
                f"stale artifact: capture APK {actual or '<missing>'} "
                f"!= expected {expect_app_sha256.lower()}")
    source = corpus["source"]
    captured = capture["corpus"]
    if captured.get("source_sha256") != source["sha256"]:
        problems.append("capture source digest does not match the pinned corpus")
    if captured.get("source_bytes") != source["bytes"]:
        problems.append("capture source size does not match the pinned corpus")
    return problems


def group(capture: dict) -> dict[tuple[str, str], list[dict]]:
    grouped: dict[tuple[str, str], list[dict]] = {}
    for sample in capture["samples"]:
        grouped.setdefault((sample["cell"], sample["format"]), []).append(sample)
    return grouped


def identity_findings(capture: dict, corpus: dict) -> list[str]:
    """C0/C3/C4 all say 'these repeats agree'; only the digest compared differs."""
    findings: list[str] = []
    container_policy = corpus.get("container_identity", {})
    for (cell, fmt), samples in sorted(group(capture).items()):
        if len(samples) < 2:
            continue
        checks = [
            (C0, "engine_sample_sha256"),
            (C3, "decoded_sample_sha256"),
            (C3, "normalized_metadata_sha256"),
        ]
        if container_policy.get(fmt) == "gated":
            checks.append((C4, "container_sha256"))
        for level, field in checks:
            # A cache hit never invokes the engine, so it carries no engine sample. Counting
            # its empty digest as a second distinct value would report a C0 break for the very
            # behaviour the cache exists to provide.
            digests = {sample[field] for sample in samples if sample.get(field)}
            if len(digests) > 1:
                findings.append(
                    f"{level} broken for {cell}/{fmt}: {len(digests)} distinct "
                    f"{field} across {len(samples)} runs")
        findings.extend(cache_fidelity_findings(
            cell, fmt, samples, container_policy.get(fmt) == "gated"))
    return findings


def cache_fidelity_findings(
    cell: str, fmt: str, samples: list[dict], container_gated: bool,
) -> list[str]:
    """A cached export must be byte-identical to the render it stands in for.

    This is the whole promise of the content-addressed cache (#179), and the one failure that
    would be invisible in a wall-time table: a hit that publishes different bytes is a silently
    wrong image, delivered faster.

    The decoded SAMPLES are compared unconditionally, because the pixels must match whatever the
    format. The whole CONTAINER is compared only where the corpus gates container identity: a
    JPEG's bytes are an encoder artifact, so freshly rendered ones may legitimately differ from
    each other, and comparing a byte-copied cache hit against that moving target would report a
    fault that is not there.
    """
    cached = [s for s in samples if s.get("served_from_cache")]
    rendered = [s for s in samples if not s.get("served_from_cache")]
    if not cached or not rendered:
        return []
    fields = ["decoded_sample_sha256"]
    if container_gated:
        fields.append("container_sha256")
    findings = []
    for field in fields:
        served = {s[field] for s in cached if s.get(field)}
        fresh = {s[field] for s in rendered if s.get(field)}
        if served and fresh and served != fresh:
            findings.append(
                f"cache served different bytes for {cell}/{fmt}: {field} "
                f"{sorted(served)} from cache vs {sorted(fresh)} rendered")
    return findings


def reconciliation_findings(capture: dict, tolerance_ms: int = 250) -> list[str]:
    findings = []
    for sample in capture["samples"]:
        gap = sample["total_ms"] - sample["phases_sum_ms"]
        if gap < -1 or gap > tolerance_ms:
            findings.append(
                f"stage reconciliation gap {gap} ms on {sample['cell']}/"
                f"{sample['format']} run {sample['run_index']} "
                f"(total {sample['total_ms']}, phases {sample['phases_sum_ms']})")
    return findings


def environment_findings(capture: dict, corpus: dict) -> list[str]:
    tier = corpus["protocol"]["tier_a"]
    findings = []
    low_battery = 101
    device = capture["device"]
    if device.get("model") != tier["model"]:
        findings.append(f"device {device.get('model')} is not Tier A ({tier['model']})")
    if device.get("sdk_int") != tier["sdk"]:
        findings.append(f"API {device.get('sdk_int')} is not Tier A (API {tier['sdk']})")
    for sample in capture["samples"]:
        env = sample["environment"]
        if tier.get("unplugged") and env.get("plugged", 0) not in (0, -1):
            findings.append(
                f"{sample['cell']}/{sample['format']} run {sample['run_index']} ran plugged in")
        if env.get("battery_pct", -1) != -1 and env["battery_pct"] < tier["min_battery_pct"]:
            # One finding for the whole run, naming the worst reading: a long capture drains
            # several points, and one line per level buries everything else.
            low_battery = min(low_battery, env["battery_pct"])
        if env.get("thermal_status", 0) > 1:
            findings.append(
                f"thermal status {env['thermal_status']} during {sample['cell']}/"
                f"{sample['format']} run {sample['run_index']}")
        # A sample that could not reach the declared thermal state before starting is not
        # comparable with one that did, however long the harness waited for it.
        wait = sample.get("thermal_wait") or {}
        if wait.get("timed_out"):
            findings.append(
                f"{sample['cell']}/{sample['format']} run {sample['run_index']} started at "
                f"thermal {wait.get('start_status')} after waiting "
                f"{int(wait.get('waited_ms', 0)) // 1000}s for {wait.get('required')}")
    if low_battery <= 100:
        findings.append(
            f"battery fell to {low_battery}%, below the {tier['min_battery_pct']}% floor")
    # Device-level conditions (model, API) are the same fact seen once per sample; repeating
    # one line 44 times buries the findings that name a specific run.
    return list(dict.fromkeys(findings))


def slo_findings(capture: dict, corpus: dict) -> list[str]:
    """The SLO binds the warm path of the gated cell/format only (issue #126)."""
    protocol = corpus["protocol"]
    gated_cell = next((c["id"] for c in corpus["cells"] if "SLO" in c.get("gates", [])), None)
    if gated_cell is None:
        return []
    gated_format = protocol.get("slo_format", "JPEG_Q95")
    # "warm" already excludes the protocol's discarded first run: the harness marks the
    # process's first render cold, which is exactly the sample the protocol throws away.
    # served_from_cache additionally separates the path the SLO actually binds (the
    # pre-rendered/cache-hit export of #179) from a warm full re-render, which is a
    # different measurement and must never be reported as meeting the SLO.
    samples = [s for s in capture["samples"]
               if s["cell"] == gated_cell and s["format"] == gated_format
               and s["state"] == "warm" and s.get("served_from_cache") is True]
    if not samples:
        return [f"no cache-hit {gated_cell}/{gated_format} sample to gate the SLO against "
                f"(the pre-rendered export path is issue #179); full re-render times are "
                f"reported above and are not an SLO result"]
    times = [float(s["total_ms"]) for s in sorted(samples, key=lambda s: s["run_index"])]
    findings = []
    # An SLO claim is a p95 claim, so it needs its own, larger sample count: the baseline
    # matrix (gate_runs) only has to pin a stable median for ordering optimization work.
    slo_runs = protocol.get("slo_runs", protocol["gate_runs"])
    if len(times) < slo_runs - protocol["gate_runs_discarded"]:
        findings.append(
            f"{len(times)} warm {gated_cell}/{gated_format} runs is below the "
            f"{slo_runs}-run SLO protocol")
    p50, p95 = percentile(times, 50), percentile(times, 95)
    if p50 > protocol["slo_p50_ms"]:
        findings.append(f"p50 {p50:.0f} ms exceeds {protocol['slo_p50_ms']} ms")
    if p95 > protocol["slo_p95_ms"]:
        findings.append(f"p95 {p95:.0f} ms exceeds {protocol['slo_p95_ms']} ms")
    return findings


def journey_findings(capture: dict) -> list[str]:
    journeys = capture.get("journeys", {})
    findings = []
    cancellation = str(journeys.get("cancellation", ""))
    if not cancellation.startswith("cancelled") and cancellation != "completed_before_cancel":
        findings.append(f"cancellation journey: {cancellation or '<missing>'}")
    if journeys.get("foreground_during_export") is not True:
        findings.append("export did not run at foreground importance")
    if journeys.get("reopen_published") != "pass":
        findings.append(f"reopen/share journey: {journeys.get('reopen_published', '<missing>')}")
    return findings


def discard_first(samples: list[dict], count: int) -> list[dict]:
    """Drop the warm-up runs when no sample identified itself as the cold one."""
    ordered = sorted(samples, key=lambda s: s["run_index"])
    return ordered[count:] if len(ordered) > count else ordered


def grade_note(capture: dict) -> str:
    """State whether the post-engine grade ran, in the report itself.

    ColorGrade.applyInPlace returns immediately when saturation, vibrance and gamut
    compression are all neutral, and the built-in presets leave them so — but a harness
    that passes a non-neutral constant silently adds a full per-pixel Oklab round-trip
    (~4.8 s at 12 MP) to every measurement. That cost is invisible in a wall-time table,
    so the capture must say which path it measured rather than leave it to be inferred.
    """
    recorded = [s.get("grade_inputs") for s in capture["samples"] if s.get("grade_inputs")]
    if not recorded:
        return "- grade inputs: not recorded (capture predates the grade_inputs field)"
    active = sorted({str(g) for g in recorded if g.get("active")})
    if not active:
        return ("- grade: neutral for every sample, so the post-engine Oklab pass is "
                "skipped - as it is on a default export")
    return ("- grade: **ACTIVE** on some samples " + ", ".join(active) +
            " - these totals include the per-pixel Oklab pass a neutral export skips")


def render(capture: dict, corpus: dict) -> str:
    lines = []
    app, device = capture["app"], capture["device"]
    protocol = capture["protocol"]
    lines.append(f"# Export benchmark capture ({'SMOKE' if protocol.get('smoke') else 'BASELINE'})")
    lines.append("")
    lines.append(f"- app `{app.get('version_name')}` ({app.get('version_code')}), "
                 f"APK `{str(app.get('apk_sha256', ''))[:16]}...`")
    lines.append(f"- device {device.get('manufacturer')} {device.get('model')}, "
                 f"API {device.get('sdk_int')} ({device.get('release')}), "
                 f"{device.get('cpu_count')} cores")
    lines.append(f"- source `{str(capture['corpus'].get('source_sha256', ''))[:16]}...` "
                 f"{capture['corpus'].get('width')}x{capture['corpus'].get('height')}")
    lines.append("")
    lines.append(grade_note(capture))
    lines.append("")
    lines.append("| cell | format | n | p50 ms | p95 ms | mean +/- 95% CI | "
                 "peak PSS MB | peak RSS MB |")
    lines.append("|---|---|---:|---:|---:|---|---:|---:|")
    for (cell, fmt), samples in sorted(group(capture).items()):
        warm = [s for s in samples if s["state"] == "warm"]
        kept = warm or discard_first(samples, corpus["protocol"]["gate_runs_discarded"])
        times = [float(s["total_ms"]) for s in kept]
        mean, half = mean_ci95(times)
        pss = max(int(s["memory"].get("total_pss_kb", 0)) for s in samples) / 1024.0
        hwm = max(int(s["memory"].get("vm_hwm_kb", 0)) for s in samples) / 1024.0
        ci = "n/a" if math.isnan(half) else f"{mean:.0f} +/- {half:.0f}"
        rss = f"{hwm:.0f}" if hwm > 0 else "-"
        lines.append(f"| {cell} | {fmt} | {len(times)} | {percentile(times, 50):.0f} | "
                     f"{percentile(times, 95):.0f} | {ci} | {pss:.0f} | {rss} |")
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("capture", type=pathlib.Path)
    ap.add_argument("--corpus", type=pathlib.Path, default=DEFAULT_CORPUS)
    ap.add_argument("--expect-app-sha256", default="")
    ap.add_argument("--gate", action="store_true",
                    help="fail on any contract finding, and refuse a smoke capture")
    ap.add_argument("--markdown", type=pathlib.Path, help="write the report table here")
    args = ap.parse_args(argv)

    corpus = json.loads(args.corpus.read_text(encoding="utf-8"))
    if corpus.get("schema") != CORPUS_SCHEMA:
        print(f"unexpected corpus schema {corpus.get('schema')!r}", file=sys.stderr)
        return 2
    try:
        capture = load(args.capture)
        blocking = check_artifact(capture, corpus, args.expect_app_sha256 or None)
    except ReportError as failure:
        print(f"bench-report: {failure}", file=sys.stderr)
        return 2
    if blocking:
        for problem in blocking:
            print(f"bench-report: {problem}", file=sys.stderr)
        return 2

    findings = (identity_findings(capture, corpus)
                + reconciliation_findings(capture)
                + journey_findings(capture))
    if args.gate:
        findings += environment_findings(capture, corpus) + slo_findings(capture, corpus)

    report = render(capture, corpus)
    print(report, end="")
    if args.markdown:
        args.markdown.write_text(report, encoding="utf-8")

    if args.gate and capture["protocol"].get("smoke"):
        print("bench-report: refusing to gate on a smoke capture "
              f"({capture['protocol'].get('requested_runs')} runs < "
              f"{capture['protocol'].get('gate_runs')})", file=sys.stderr)
        return 2
    for finding in findings:
        print(f"bench-report: {finding}", file=sys.stderr)
    if findings:
        return 1
    print("bench-report: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

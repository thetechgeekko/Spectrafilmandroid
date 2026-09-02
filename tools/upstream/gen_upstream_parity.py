#!/usr/bin/env python3
"""Generate and verify the upstream parity manifest (#189).

The manifest (`parity_manifest.json`) classifies every reviewed upstream item; this
tool renders `docs/UPSTREAM_PARITY.md` from it and, in `--check` mode, verifies —
fully offline — that the schema holds, every item carries exactly one status with
the evidence its status requires, the bundled asset tree still matches the pinned
digest, and the committed document is exactly what the manifest generates.

    python tools/upstream/gen_upstream_parity.py --write     # (re)generate the doc
    python tools/upstream/gen_upstream_parity.py --check     # CI: offline drift gate
    python tools/upstream/gen_upstream_parity.py --report-upstream  # never fails

`--report-upstream` asks GitHub whether upstream main moved past the reviewed pin.
It reports; it never mutates the pin and never fails the build (a read-only drift
report must not turn network weather into a red CI).

Film modeling powered by spektrafilm (GPLv3).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import urllib.request

TOOL_DIR = pathlib.Path(__file__).resolve().parent
REPO = TOOL_DIR.parents[1]
PIN = TOOL_DIR / "upstream_pin.json"
MANIFEST = TOOL_DIR / "parity_manifest.json"
DOC = REPO / "docs" / "UPSTREAM_PARITY.md"

PIN_SCHEMA = "spk.upstream_pin.v1"
MANIFEST_SCHEMA = "spk.upstream_parity.v1"

# One status per item. "inapplicable" covers both desktop-only tooling and upstream
# changes ruled out by the owner-approved baseline (oracle pin / Strategy A) — the
# rationale field says which.
STATUSES = ("matched", "adapted", "extension", "inapplicable", "missing")
AREAS = ("stages", "parameters", "profiles-assets", "gui", "lut-formats",
         "android-extensions", "upstream-delta", "experimental-branches")


def load_json(path: pathlib.Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def asset_tree_sha256(root: pathlib.Path) -> tuple[str, int]:
    """Digest of (relative path, git blob hash) pairs in sorted order — one number that
    moves if any bundled asset is added, removed, renamed, or edited.

    The per-file hash is `git hash-object`, i.e. the content as git would COMMIT it
    (clean filters applied), so a Windows checkout with CRLF text files produces the
    same digest as the LF checkout on a Linux CI runner. Raw working-tree bytes are
    exactly the thing that differs between the two."""
    # Sort by the POSIX relative string: WindowsPath ordering is case-insensitive,
    # so sorting Path objects puts mixed-case ICC filenames in a different order
    # than on Linux and the outer digest diverges per platform.
    entries = sorted((p for p in root.rglob("*") if p.is_file()),
                     key=lambda p: p.relative_to(root).as_posix())
    listing = "\n".join(str(p) for p in entries) + "\n"
    proc = subprocess.run(
        ["git", "hash-object", "--stdin-paths"],
        input=listing, capture_output=True, text=True, check=True, cwd=REPO)
    hashes = proc.stdout.split()
    if len(hashes) != len(entries):
        raise RuntimeError(
            f"git hash-object returned {len(hashes)} hashes for {len(entries)} files")
    outer = hashlib.sha256()
    for path, blob in zip(entries, hashes):
        rel = path.relative_to(root).as_posix()
        outer.update(f"{rel}\n{blob}\n".encode("utf-8"))
    return outer.hexdigest(), len(entries)


def validate(pin: dict, manifest: dict) -> list[str]:
    problems: list[str] = []
    if pin.get("schema") != PIN_SCHEMA:
        problems.append(f"pin schema {pin.get('schema')!r} != {PIN_SCHEMA}")
    if manifest.get("schema") != MANIFEST_SCHEMA:
        problems.append(f"manifest schema {manifest.get('schema')!r} != {MANIFEST_SCHEMA}")
    if manifest.get("oracle_sha") != pin.get("oracle", {}).get("sha"):
        problems.append("manifest oracle_sha disagrees with the pin")
    if manifest.get("reviewed_sha") != pin.get("reviewed", {}).get("sha"):
        problems.append("manifest reviewed_sha disagrees with the pin")

    seen: set[str] = set()
    for item in manifest.get("items", ()):
        ident = item.get("id", "<missing id>")
        if ident in seen:
            problems.append(f"duplicate item id {ident}")
        seen.add(ident)
        for field in ("id", "area", "upstream", "status", "evidence"):
            if not item.get(field):
                problems.append(f"{ident}: missing field {field!r}")
        if item.get("area") not in AREAS:
            problems.append(f"{ident}: unknown area {item.get('area')!r}")
        if item.get("status") not in STATUSES:
            problems.append(f"{ident}: unknown status {item.get('status')!r}")
        if item.get("status") == "missing" and not item.get("ticket"):
            problems.append(f"{ident}: missing item without an atomic ticket link")
        if item.get("status") == "inapplicable" and not item.get("rationale"):
            problems.append(f"{ident}: inapplicable without a rationale")
    return problems


def render(pin: dict, manifest: dict) -> str:
    o, r = pin["oracle"], pin["reviewed"]
    lines = [
        "# Upstream parity manifest",
        "",
        "> GENERATED by `tools/upstream/gen_upstream_parity.py` from",
        "> `tools/upstream/parity_manifest.json` — edit the manifest, not this file.",
        "> CI regenerates and diffs it (`--check`), so hand edits fail the build.",
        "",
        "This is the current, versioned upstream-coverage view required by",
        "[#189](https://github.com/thetechgeekko/Spektrafilm-android/issues/189).",
        "Dated sync/porting plans (e.g. `UPSTREAM_SYNC_2026-06-24.md`) are historical",
        "input, not coverage claims.",
        "",
        "## Pins",
        "",
        f"- **Upstream:** [{pin['repository']}]({pin['repository']}) ({pin['license']})",
        f"- **Oracle (numeric contract):** `{o['sha'][:7]}` — {o['subject']} ({o['date']}).",
        "  Every committed golden reproduces bit-exactly here and only here; the",
        f"  first diverging child is `{o['drift_child']['sha'][:7]}`.",
        f"- **Reviewed upstream main:** `{r['sha'][:7]}` (tree `{r['tree'][:7]}`) — "
        f"{r['subject']} ({r['date']}), reviewed {r['review_date']}.",
        f"- **Bundled assets:** {manifest['assets']['files']} files under "
        f"`{pin['assets']['path']}`, tree digest `{manifest['assets']['tree_sha256'][:16]}…`",
        "- **Track-only branches:** "
        + ", ".join(f"`{name}@{sha[:7]}`"
                    for name, sha in sorted(pin["track_only_branches"].items())
                    if name != "note") + ".",
        "",
        "## Status vocabulary",
        "",
        "| Status | Meaning |",
        "|---|---|",
        "| matched | 1:1 with the oracle revision; parity-gated |",
        "| adapted | Same capability, deliberately different mechanism or defaults on Android |",
        "| extension | Android-only; no upstream counterpart |",
        "| inapplicable | Not applicable here — desktop-only, or ruled out by the pinned baseline; rationale given |",
        "| missing | Applicable and absent; owned by the linked atomic ticket |",
        "",
    ]
    counts: dict[str, int] = {}
    for item in manifest["items"]:
        counts[item["status"]] = counts.get(item["status"], 0) + 1
    lines.append("**Totals:** " + ", ".join(
        f"{counts.get(s, 0)} {s}" for s in STATUSES) + f" ({len(manifest['items'])} items).")
    lines.append("")

    titles = {
        "stages": "Runtime stages and taps",
        "parameters": "Parameters and defaults",
        "profiles-assets": "Profiles and assets",
        "gui": "GUI-visible behavior",
        "lut-formats": "LUT creation, import and export formats",
        "android-extensions": "Android-only extensions",
        "upstream-delta": "Reviewed upstream delta (c1d0e44 → 3bb2c2d)",
        "experimental-branches": "Experimental upstream branches",
    }
    for area in AREAS:
        items = [i for i in manifest["items"] if i["area"] == area]
        if not items:
            continue
        lines.append(f"## {titles[area]}")
        lines.append("")
        lines.append("| Upstream item | Status | Android / evidence |")
        lines.append("|---|---|---|")
        for item in items:
            evidence = item["evidence"]
            if item.get("rationale"):
                evidence += f" — *{item['rationale']}*"
            if item.get("ticket"):
                evidence += f" — ticket {item['ticket']}"
            lines.append(f"| {item['upstream']} | {item['status']} | {evidence} |")
        lines.append("")
    lines.append("*Film modeling powered by spektrafilm (GPLv3).*")
    lines.append("")
    return "\n".join(lines)


def report_upstream(pin: dict) -> None:
    url = ("https://api.github.com/repos/andreavolpato/spektrafilm/branches/main")
    pinned = pin["reviewed"]["sha"]
    try:
        with urllib.request.urlopen(url, timeout=10) as response:
            head = json.load(response)["commit"]["sha"]
    except Exception as failure:  # noqa: BLE001 - a drift REPORT never fails the build
        print(f"upstream-report: could not reach GitHub ({failure}); pin unchanged")
        return
    if head == pinned:
        print(f"upstream-report: upstream main is still the reviewed pin {pinned[:7]}")
    else:
        print(f"upstream-report: NOTICE upstream main moved to {head[:7]} "
              f"(reviewed pin {pinned[:7]}). Review the delta and update the pin "
              "deliberately; nothing is changed automatically.")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true", help="regenerate the doc + digests")
    mode.add_argument("--check", action="store_true", help="offline drift gate")
    mode.add_argument("--report-upstream", action="store_true",
                      help="network drift report; never fails")
    args = ap.parse_args(argv)

    pin = load_json(PIN)
    if args.report_upstream:
        report_upstream(pin)
        return 0

    manifest = load_json(MANIFEST)
    digest, files = asset_tree_sha256(REPO / pin["assets"]["path"])

    if args.write:
        pin["assets"]["tree_sha256"] = digest
        pin["assets"]["files"] = files
        manifest["assets"] = {"tree_sha256": digest, "files": files}
        problems = validate(pin, manifest)
        if problems:
            for problem in problems:
                print(f"upstream-parity: {problem}", file=sys.stderr)
            return 1
        PIN.write_text(json.dumps(pin, indent=2, ensure_ascii=False) + "\n",
                       encoding="utf-8", newline="\n")
        MANIFEST.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                            encoding="utf-8", newline="\n")
        DOC.write_text(render(pin, manifest), encoding="utf-8", newline="\n")
        print(f"upstream-parity: wrote {DOC.relative_to(REPO)} "
              f"({len(manifest['items'])} items, assets {files} files {digest[:16]}…)")
        return 0

    problems = validate(pin, manifest)
    if pin["assets"]["tree_sha256"] != digest or pin["assets"]["files"] != files:
        problems.append(
            f"bundled asset tree drifted: {files} files {digest[:16]}… vs pinned "
            f"{pin['assets']['files']} files {pin['assets']['tree_sha256'][:16]}…")
    if manifest.get("assets", {}).get("tree_sha256") != pin["assets"]["tree_sha256"]:
        problems.append("manifest asset digest disagrees with the pin")
    expected = render(pin, manifest)
    actual = DOC.read_text(encoding="utf-8") if DOC.exists() else ""
    if actual != expected:
        problems.append(f"{DOC.relative_to(REPO)} is not what the manifest generates "
                        "(run --write and commit)")
    for problem in problems:
        print(f"upstream-parity: {problem}", file=sys.stderr)
    if problems:
        return 1
    print(f"upstream-parity: OK ({len(manifest['items'])} items, "
          f"assets {files} files, doc in sync)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

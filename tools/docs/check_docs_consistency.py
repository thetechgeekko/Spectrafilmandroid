#!/usr/bin/env python3
"""Offline consistency checks for Spektrafilm's current authority documents."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[2]
CURRENT_DOCS = (
    ROOT / "README.md",
    ROOT / "CLAUDE.md",
    ROOT / "docs" / "EXECUTION_INDEX.md",
    ROOT / "docs" / "ARCHITECTURE.md",
    ROOT / "docs" / "PRODUCTION_READINESS_PLAN.md",
    ROOT / "docs" / "BIT_IDENTICAL_EXPORT_ROADMAP.md",
    ROOT / "docs" / "RELEASE_CHECKLIST.md",
    ROOT / "docs" / "MOBILE_STRATEGY.md",
    ROOT / "docs" / "PRESETS.md",
    ROOT / "docs" / "LICENSING.md",
    ROOT / "docs" / "JNI_LIFETIME_SAFETY.md",
    ROOT / "docs" / "TRANSACTIONAL_STORAGE.md",
    ROOT / "docs" / "RAW_DNG.md",
    ROOT / "docs" / "MASK_JSON_SCHEMA.md",
    ROOT / "engine" / "spektra-core" / "README.md",
    ROOT / "lib" / "libraw" / "README.md",
)


def _extract(pattern: str, text: str, source: Path) -> str:
    match = re.search(pattern, text, flags=re.MULTILINE)
    if not match:
        raise ValueError(f"could not derive {pattern!r} from {source.relative_to(ROOT)}")
    return match.group(1)


def _without_fenced_code(text: str) -> str:
    output: list[str] = []
    fence_char = ""
    fence_length = 0
    for line in text.splitlines(keepends=True):
        marker = re.match(r"^\s*(`{3,}|~{3,})", line)
        if marker:
            token = marker.group(1)
            if not fence_char:
                fence_char, fence_length = token[0], len(token)
            elif token[0] == fence_char and len(token) >= fence_length:
                fence_char, fence_length = "", 0
            output.append("\n" if line.endswith(("\n", "\r")) else "")
        elif fence_char:
            output.append("\n" if line.endswith(("\n", "\r")) else "")
        else:
            output.append(line)
    return "".join(output)


def _markdown_link_targets(text: str) -> list[str]:
    """Return common inline/image/reference Markdown targets outside fenced code."""
    text = _without_fenced_code(text)
    targets: list[str] = []
    cursor = 0
    while True:
        marker = text.find("](", cursor)
        if marker < 0:
            break
        start = marker + 2
        if start < len(text) and text[start] == "<":
            end = text.find(">", start + 1)
            if end >= 0 and end + 1 < len(text) and text[end + 1] == ")":
                targets.append(text[start : end + 1])
                cursor = end + 2
                continue
        depth = 1
        index = start
        while index < len(text) and depth:
            if text[index] == "\\":
                index += 2
                continue
            if text[index] == "(":
                depth += 1
            elif text[index] == ")":
                depth -= 1
            index += 1
        if depth == 0:
            targets.append(text[start : index - 1])
            cursor = index
        else:
            cursor = start
    targets.extend(
        match.group(1)
        for match in re.finditer(
            r"^\s*\[[^\]]+\]:\s*(<[^>\r\n]+>|[^\s]+)",
            text,
            flags=re.MULTILINE,
        )
    )
    return targets


def _check_local_links(path: Path, text: str) -> list[str]:
    errors: list[str] = []
    for raw_target in _markdown_link_targets(text):
        stripped = raw_target.strip()
        if stripped.startswith("<") and ">" in stripped:
            target = stripped[1 : stripped.index(">")]
        else:
            target = stripped.split(maxsplit=1)[0]
        if not target or target.startswith("#"):
            continue
        parsed = urlsplit(target)
        scheme = parsed.scheme.lower()
        if parsed.netloc or scheme in {"http", "https", "mailto"}:
            continue
        if scheme:
            errors.append(
                f"{path.relative_to(ROOT)}: unsupported link scheme {scheme!r}: {target}"
            )
            continue
        decoded_path = unquote(parsed.path)
        decoded = urlsplit(decoded_path)
        decoded_scheme = decoded.scheme.lower()
        if decoded.netloc or decoded_scheme in {"http", "https", "mailto"}:
            errors.append(
                f"{path.relative_to(ROOT)}: encoded external link target is not allowed: {target}"
            )
            continue
        if decoded_scheme:
            errors.append(
                f"{path.relative_to(ROOT)}: encoded link scheme {decoded_scheme!r}: {target}"
            )
            continue
        if len(decoded_path) >= 2 and all(char in "/\\" for char in decoded_path[:2]):
            errors.append(f"{path.relative_to(ROOT)}: unsafe network link target: {target}")
            continue
        relative = decoded_path
        resolved = (path.parent / relative).resolve()
        try:
            resolved.relative_to(ROOT)
        except ValueError:
            errors.append(f"{path.relative_to(ROOT)}: link escapes repository: {target}")
            continue
        if not resolved.exists():
            errors.append(f"{path.relative_to(ROOT)}: missing local link target: {target}")
    return errors


def _preset_set_errors(asset_ids: list[str], documented_ids: list[str]) -> list[str]:
    errors: list[str] = []
    if len(asset_ids) != len(set(asset_ids)):
        errors.append("presets.json: duplicate preset ID")
    if len(documented_ids) != len(set(documented_ids)):
        errors.append("docs/PRESETS.md: duplicate documented preset ID")
    missing = sorted(set(asset_ids) - set(documented_ids))
    extra = sorted(set(documented_ids) - set(asset_ids))
    if missing:
        errors.append(f"docs/PRESETS.md: missing preset IDs: {', '.join(missing)}")
    if extra:
        errors.append(f"docs/PRESETS.md: unknown preset IDs: {', '.join(extra)}")
    return errors


def _sdk_summary(min_sdk: str, target_sdk: str, compile_sdk: str) -> str:
    return f"min {min_sdk}, target {target_sdk}, compile {compile_sdk}"


def _included_module_gradles(settings: str) -> list[Path]:
    modules: list[str] = []
    for arguments in re.findall(r"^\s*include\(([^)]*)\)", settings, flags=re.MULTILINE):
        modules.extend(re.findall(r'["\'](:[^"\']+)["\']', arguments))
    return [
        ROOT.joinpath(*module.removeprefix(":").split(":")) / "build.gradle.kts"
        for module in modules
    ]


def _workflow_pin_errors(
    workflows: dict[Path, str], expected: dict[str, str]
) -> list[str]:
    errors: list[str] = []
    for path, text in workflows.items():
        for label, version in expected.items():
            found = set(
                re.findall(
                    rf"{re.escape(label)}[;/]([0-9.]+)",
                    text,
                    flags=re.IGNORECASE,
                )
            )
            if found != {version}:
                rendered = ", ".join(sorted(found)) or "none"
                errors.append(
                    f"{path.relative_to(ROOT)}: expected {label} {version}, found {rendered}"
                )
    return errors


def _parity_flag_errors(ci: str, release: str, shipping_flags: str) -> list[str]:
    errors: list[str] = []
    ci_flags = set(re.findall(r'^\s+opt:\s*"([^"]+)"', ci, flags=re.MULTILINE))
    required_ci_flags = {"-O2", shipping_flags}
    if not required_ci_flags.issubset(ci_flags):
        errors.append(
            "ci.yml: parity matrix must contain -O2 and the CMake shipping flags; "
            f"found {sorted(ci_flags)}"
        )
    release_flags = set(
        re.findall(
            r"^\s+SPK_PARITY_EXTRA_FLAGS:\s*([^\r\n]+?)\s*$",
            release,
            flags=re.MULTILINE,
        )
    )
    if release_flags != {shipping_flags}:
        errors.append(
            "release.yml: SPK_PARITY_EXTRA_FLAGS must equal the CMake shipping flags; "
            f"found {sorted(release_flags)}"
        )
    return errors


def main() -> int:
    errors: list[str] = []
    try:
        app_gradle = (ROOT / "app" / "build.gradle.kts").read_text(encoding="utf-8")
        workflow_paths = (
            ROOT / ".github" / "workflows" / "ci.yml",
            ROOT / ".github" / "workflows" / "release.yml",
            ROOT / ".github" / "workflows" / "r8-smoke.yml",
        )
        workflows = {path: path.read_text(encoding="utf-8") for path in workflow_paths}
        ci = workflows[workflow_paths[0]]
        engine_cmake = (ROOT / "engine" / "spektra-core" / "src" / "main" / "cpp" / "CMakeLists.txt").read_text(encoding="utf-8")
        version_catalog = (ROOT / "gradle" / "libs.versions.toml").read_text(encoding="utf-8")
        wrapper = (ROOT / "gradle" / "wrapper" / "gradle-wrapper.properties").read_text(encoding="utf-8")
        settings = (ROOT / "settings.gradle.kts").read_text(encoding="utf-8")
        libraw_vendor = (ROOT / "lib" / "libraw" / "cmake" / "LibRawVendor.cmake").read_text(encoding="utf-8")
        version_name = _extract(r'^\s*versionName\s*=\s*"([^"]+)"', app_gradle, ROOT / "app/build.gradle.kts")
        version_code = _extract(r"^\s*versionCode\s*=\s*(\d+)", app_gradle, ROOT / "app/build.gradle.kts")
        min_sdk = _extract(r"^\s*minSdk\s*=\s*(\d+)", app_gradle, ROOT / "app/build.gradle.kts")
        target_sdk = _extract(r"^\s*targetSdk\s*=\s*(\d+)", app_gradle, ROOT / "app/build.gradle.kts")
        compile_sdk = _extract(r"^\s*compileSdk\s*=\s*(\d+)", app_gradle, ROOT / "app/build.gradle.kts")
        build_tools = _extract(r'^\s*buildToolsVersion\s*=\s*"([^"]+)"', app_gradle, ROOT / "app/build.gradle.kts")
        native_gradles = []
        for gradle_path in _included_module_gradles(settings):
            gradle_text = gradle_path.read_text(encoding="utf-8")
            if "externalNativeBuild" in gradle_text:
                native_gradles.append((gradle_path, gradle_text))
        if not native_gradles:
            raise ValueError("no Gradle native modules found")
        ndk_versions = {
            _extract(r'^\s*ndkVersion\s*=\s*"([^"]+)"', text, path)
            for path, text in native_gradles
        }
        cmake_versions = {
            _extract(r'^\s*version\s*=\s*"([^"]+)"', text, path)
            for path, text in native_gradles
        }
        if len(ndk_versions) != 1:
            raise ValueError(f"native modules disagree on NDK pins: {sorted(ndk_versions)}")
        if len(cmake_versions) != 1:
            raise ValueError(f"native modules disagree on CMake pins: {sorted(cmake_versions)}")
        ndk_version = next(iter(ndk_versions))
        cmake_version = next(iter(cmake_versions))
        shipping_flags = _extract(r'^set\(CMAKE_CXX_FLAGS_RELEASE\s+"([^"]+)"\)', engine_cmake, ROOT / "engine/spektra-core/src/main/cpp/CMakeLists.txt")
        agp_version = _extract(r'^agp\s*=\s*"([^"]+)"', version_catalog, ROOT / "gradle/libs.versions.toml")
        kotlin_version = _extract(r'^kotlin\s*=\s*"([^"]+)"', version_catalog, ROOT / "gradle/libs.versions.toml")
        gradle_version = _extract(r"gradle-([0-9.]+)-bin\.zip", wrapper, ROOT / "gradle/wrapper/gradle-wrapper.properties")
        libraw_version = _extract(r'^set\(SFRAW_PINNED_LIBRAW_VERSION\s+"([^"]+)"\)', libraw_vendor, ROOT / "lib/libraw/cmake/LibRawVendor.cmake")
        parity_count = len(re.findall(r"^\s+build_run\s+test_", ci, flags=re.MULTILINE))
        preset_asset = json.loads(
            (ROOT / "engine" / "spektra-core" / "src" / "main" / "assets" / "spektra" / "presets.json").read_text(
                encoding="utf-8"
            )
        )
        asset_preset_ids = [str(item["id"]) for item in preset_asset["presets"]]
        preset_doc_text = (ROOT / "docs" / "PRESETS.md").read_text(encoding="utf-8")
        documented_preset_ids = re.findall(
            r"^### .+?\(`([^`]+)`\)\s*$", preset_doc_text, flags=re.MULTILINE
        )
    except (OSError, ValueError) as exc:
        print(f"docs-consistency: ERROR: {exc}", file=sys.stderr)
        return 1

    required_fragments = {
        ROOT / "README.md": (
            "docs/EXECUTION_INDEX.md",
            "oracle tolerance",
            "Earlier owner status note",
        ),
        ROOT / "CLAUDE.md": ("docs/EXECUTION_INDEX.md", f"{parity_count} tests"),
        ROOT / "docs" / "EXECUTION_INDEX.md": (
            f"`{version_name}` / versionCode `{version_code}`",
            _sdk_summary(min_sdk, target_sdk, compile_sdk),
            f"`{build_tools}`",
            f"AGP `{agp_version}`, Kotlin `{kotlin_version}`, Gradle `{gradle_version}`",
            f"NDK `{ndk_version}`, CMake `{cmake_version}`",
            f"LibRaw `{libraw_version}`",
            shipping_flags,
            f"{parity_count} cases",
        ),
        ROOT / "docs" / "PRODUCTION_READINESS_PLAN.md": ("EXECUTION_INDEX.md",),
        ROOT / "docs" / "BIT_IDENTICAL_EXPORT_ROADMAP.md": ("EXECUTION_INDEX.md",),
        ROOT / "docs" / "RELEASE_CHECKLIST.md": ("EXECUTION_INDEX.md",),
    }
    forbidden_fragments = {
        ROOT / "README.md": ("checked bit-for-bit",),
        ROOT / "docs" / "MOBILE_STRATEGY.md": (
            "GPU is an **optional accelerator for the preview path only**",
            "export always uses the CPU engine",
        ),
    }

    errors.extend(_preset_set_errors(asset_preset_ids, documented_preset_ids))
    errors.extend(
        _workflow_pin_errors(
            workflows,
            {
                "ndk": ndk_version,
                "cmake": cmake_version,
                "build-tools": build_tools,
            },
        )
    )
    errors.extend(
        _parity_flag_errors(
            ci,
            workflows[ROOT / ".github" / "workflows" / "release.yml"],
            shipping_flags,
        )
    )

    for path in CURRENT_DOCS:
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as exc:
            errors.append(f"{path.relative_to(ROOT)}: {exc}")
            continue
        errors.extend(_check_local_links(path, text))
        for fragment in required_fragments.get(path, ()):
            if fragment not in text:
                errors.append(f"{path.relative_to(ROOT)}: missing required text {fragment!r}")
        for fragment in forbidden_fragments.get(path, ()):
            if fragment in text:
                errors.append(f"{path.relative_to(ROOT)}: stale policy text {fragment!r}")

    if errors:
        print(f"docs-consistency: FAILED ({len(errors)} error(s))")
        for error in errors:
            print(f"  {error}")
        return 1

    print(
        "docs-consistency: OK "
        f"(v{version_name}/{version_code}, SDK {min_sdk}/{target_sdk}/{compile_sdk}, "
        f"AGP/Kotlin/Gradle {agp_version}/{kotlin_version}/{gradle_version}, "
        f"NDK/CMake {ndk_version}/{cmake_version}, build-tools {build_tools}, "
        f"LibRaw {libraw_version}, flags {shipping_flags!r}, parity {parity_count}, "
        f"presets {len(asset_preset_ids)})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

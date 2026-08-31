# Licensing

<!-- libraw-license-route: UNRESOLVED -->

LibRaw Android distribution route: UNRESOLVED.

> **Open release blocker (2026-08-29):** LibRaw is statically compiled into the
> native RAW module. Do not use this document, the compliance bundle, or a green
> automated verifier as legal approval until
> [Resolve LibRaw static-link compliance and publish a complete license/source bundle](https://github.com/thetechgeekko/Spektrafilm-android/issues/166)
> closes and this file, `NOTICE.md`, the in-app notices, SBOM, selected-route
> marker, and release bundle are reviewed and updated together.

## Summary

The combined application is distributed under **GPL-3.0-only**. No repository
grant of the GPL "or any later version" option has been identified.

| Component | Upstream license | Compatibility |
|-----------|------------------|---------------|
| spektrafilm (engine we port) | **GPL-3.0** | Defines the floor: any derivative must be GPLv3. |
| ImageToolbox (originally planned host; never vendored — row kept in case its code is ever incorporated) | **Apache-2.0** | Apache-2.0 → GPLv3 is **one-way compatible**; Apache code may be incorporated into a GPLv3 work. |
| LibRaw (RAW decode) | Offered under a choice of **LGPL-2.1-only** or **CDDL-1.0** | Route is `UNRESOLVED`; including both texts is provenance, not election. CDDL-1.0 is not generally treated as GPL-compatible. Static-distribution obligations require human review. |

Result: **GPLv3** is the only license that satisfies all constraints. `LICENSE` is the GPLv3
text; `NOTICE.md` carries attributions.

## Why GPLv3 (not a choice)

spektrafilm is GPLv3 and its README is explicit: *"any derivative work must also be open source
under the same license. Derivative work includes any software, plugin, or tool that incorporates
spektrafilm code or is directly inspired by its methods."* Since `spektra-core` is a direct port
of spektrafilm, the engine — and therefore the app that links it — must be GPLv3.

## Apache-2.0 → GPLv3 direction

The Apache Software Foundation and FSF agree Apache-2.0 is compatible with GPLv3 (but **not**
GPLv2). Incorporating ImageToolbox (Apache-2.0) into this GPLv3 work is allowed; we retain
ImageToolbox's copyright headers and `LICENSE`/`NOTICE` references in the files that originate
from it. The combined/derived whole is offered under GPLv3.

## LibRaw

LibRaw is currently compiled as a static native library and included in the RAW JNI module. The
project has not yet resolved which dual-license route and static-distribution materials it will use;
`lib/libraw/compliance/license-route.txt` therefore remains `UNRESOLVED`, and
the release workflow must reject that state. CI may construct and verify the
route-neutral bundle so packaging regressions are caught before the decision.
The bundle includes both upstream license texts for provenance; this does not
select either route or establish compatibility. The paired canonical
`license-decision.json` keeps the owner, decision date, rationale, HTTPS approval
reference, and local-patch contribution authorization null/false until a human
rights holder records them; release-mode audit rejects a marker-only change.
`spdx-created-at.txt` is the checked-in SPDX document creation time and must be
canonical UTC, not future-dated, and no earlier than the decision's `recorded_at`.
If we later enable the Adobe **DNG SDK**
add-on for non-baseline DNGs, we will separately review and record its exact terms.

## Practical obligations

- Ship `LICENSE` (GPLv3) and `NOTICE.md` in the repo and in-app (the host already has a
  "libraries info" screen we extend).
- Publish the exact release source, patches, notices, SBOM, and reproducible
  source/relink materials required by the human-selected LibRaw route. A public
  repository alone is not recorded here as satisfying that route.
- Preserve upstream copyright/license notices in inherited files.
- Credit: *"film modeling powered by spektrafilm"* in app About/credits, per upstream request.

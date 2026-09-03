# Export cache and idle pre-render — device evidence (#179)

Device SM-S948W (Tier A hardware), API 36, unplugged for every sample, release build.
Captured 2026-09-02 and 2026-09-03. Raw capture: [`capture.json`](capture.json); host report:
[`slo-report.md`](slo-report.md); gate output: [`gate-findings.txt`](gate-findings.txt).

## 1. Container cache — the warm export SLO

`tools/baseline/run_bench.sh <apk> 11 BASE` on APK `1fac6a9da5a1f7e4…`, 44 samples:
one render per format (the cold cache) and ten cache hits each, with the protocol's 60 s idle
before every sample.

| format | render (run 0) | cache hits, n = 10 | p50 | p95 |
|---|---:|---|---:|---:|
| **JPEG_Q95** (SLO) | 6279 ms | 24 … 67 ms | **35 ms** | **67 ms** |
| ULTRA_HDR | 6216 ms | 26 … 58 ms | 40 ms | 58 ms |
| PNG16 | 7392 ms | 93 … 1215 ms | 124 ms | 1215 ms |
| TIFF16 | 6402 ms | 293 … 391 ms | 359 ms | 391 ms |

The #126 SLO binds BASE/JPEG_Q95 on the warm path: **p50 35 ms against 2000 ms, p95 67 ms
against 3000 ms** — inside by ~57× and ~45×.

**Exactness.** Across all 11 samples of every format there is exactly **one** distinct
`container_sha256` and **one** distinct `decoded_sample_sha256`. The bytes a hit publishes are
the bytes the render produced, for the two formats whose containers are gated (PNG16, TIFF16)
as well as the two whose are not.

The PNG16 outlier (1215 ms) is the first hit after the render, with the payload not yet in page
cache. Every later PNG16 hit is 93–134 ms.

### The idle matters, and it was never applied before

These numbers are the first taken under the idle the protocol declares. `Ticket177BenchmarkChecks`
read `idle_between_runs_s` from the protocol root while the corpus declares it under
`protocol.tier_a`, so `optInt`'s default had silently made every previous capture a zero-idle
one. Back-to-back, a PNG16 hit measures **70 ms**; after a 60 s idle, **1215 ms**, because the
page cache is cold and the cores have clocked down. A cache hit is still far inside the SLO
either way, but the earlier figure was a hot best case, not what a user experiences.

## 2. Idle pre-render — payload fidelity

`ticket177_phase prerender` on APK `ea390f9397c49633…`: render once, encode the live engine
result, encode the payload written to disk and mapped back, compare the containers under a
pinned clock.

| format | live | restored | payload write | restore + encode |
|---|---|---|---:|---:|
| JPEG_Q95 | `70145513a444` | `70145513a444` | 97 ms | **272 ms** |
| PNG16 | `0cd14d142b35` | `0cd14d142b35` | 68 ms | 1969 ms |
| TIFF16 | `39f29adbf7f7` | `39f29adbf7f7` | 91 ms | 1965 ms |

Payload 149 817 600 bytes (12.5 MP × 3 × float32). The container digests are identical, so the
native TIFF/PNG16 writers read a `MappedByteBuffer` across JNI exactly as they read an engine
allocation — the property the JVM tests cannot reach.

Rerun on the current build (`6b9f193663d0fe88…`, HEAD `b0e032d`) after the #175 writer work:
digests still identical live-vs-restored, and restore+encode is **261 / 781 / 540 ms** for
JPEG / PNG16 / TIFF16 — the PNG16 and TIFF16 figures are 2.5× and 3.6× faster than the row
above, and PNG16's digest moved to `74237c9d8543` because the banded deflate re-baselined the
container under #126 C4 (decoded pixels unchanged). See
[`prerender-identity.txt`](prerender-identity.txt).

A first export served from a payload is therefore **~0.27 s (JPEG) / ~2.0 s (PNG16, TIFF16)**
against 6.2–7.4 s rendered.

## 3. Editor wiring

With the editor open on a 12 MP source and left alone, the release build logs

```
I Spektra: stage timings ms [export id=2]: preprocess=153.0 … grain=2337.9 … glare_field=205.7
I Spektra: pre-rendered export in 9396 ms
```

so the 5 s idle trigger fires, renders at full resolution and stores the payload. (9.4 s because
the recipe on screen carried grain, halation and DIR couplers.)

**Not yet verified on a device:** an export *consuming* that payload through the editor's own
export path. It needs a UI export on the owner's phone, which writes to their gallery; the
instrumentation above proves the payload round-trip and the store, and both call sites build
the key from the same `decodeIdentityOf` helper, but the editor-to-export hit itself is
unproven.

## 4. Gate status — the clean Tier A capture

The 2026-09-02 capture above **failed** the Tier A gate. Every finding was environmental,
none was about the measurements:

```
thermal status 2 during BASE/TIFF16 run 5
BASE/TIFF16 run 5 started at thermal 2 after waiting 300s for 0
BASE/ULTRA_HDR run 5 started at thermal 1 after waiting 300s for 0
battery fell to 28%, below the 50% floor
```

It has now been rerun on a charged, cool device and **passes**: `bench-report: OK`, no
findings. 2026-09-03, APK `6b9f193663d0fe88…` (HEAD `b0e032d`), 44 samples, unplugged for
every one of them (`plugged: 0`), battery **79% → 71%** against the 50% floor, and the
per-sample thermal wait never had to give up. Raw capture:
[`capture-gate-clean.json`](capture-gate-clean.json); report:
[`report-gate-clean.md`](report-gate-clean.md).

| format | render (the one miss) | cache hits, n = 10 | p50 | p95 |
|---|---:|---|---:|---:|
| **JPEG_Q95** (SLO) | 5831 ms | 29 … 68 ms | **32 ms** | **68 ms** |
| ULTRA_HDR | 6458 ms | 32 … 77 ms | 51 ms | 77 ms |
| PNG16 | 6016 ms | 79 … 133 ms | 106 ms | 133 ms |
| TIFF16 | 12030 ms | 295 … 413 ms | 340 ms | 413 ms |

The #126 SLO binds BASE/JPEG_Q95 on the cache-hit path, and the gate evaluates exactly that
subset (10 warm hits, meeting the 11-run protocol's 10 after its discarded first run):
**p50 32 ms against 2000 ms, p95 68 ms against 3000 ms** — inside by ~63× and ~44×.

**Exactness, again and on a different build.** One distinct `container_sha256` and one
distinct `decoded_sample_sha256` per format across all 11 samples. These are the same digests
the #175 captures recorded on two earlier builds, so the cache hit publishes the bytes the
render produced, and does so identically across three independent builds of the app.

Peak RSS 1346 MB, i.e. bounded and below the 1.5–1.8 GB the ungated runs reached.

Two differences from the earlier capture are worth naming, because both are the protocol
working rather than noise:

- **The PNG16 1215 ms first-hit outlier did not recur**: every PNG16 hit here lands in
  93–133 ms, so p95 is 133 ms rather than 1215 ms.
- **TIFF16's miss is 12030 ms, and that is throttling, not TIFF.** It is the third full 12 MP
  render inside run 0, which the protocol runs back to back (the 60 s idle sits between runs,
  not between the formats of one run), and it is the only sample reporting
  `thermal_status: 1`. Every phase roughly doubled together — decode 961 → 1847 ms, simulate
  4644 → 9427 ms, encode 399 → 721 ms — which is what a clock drop looks like and is not
  something any single stage can cause. It is a miss, so it is not an SLO sample.

*Film modeling powered by spektrafilm (GPLv3).*
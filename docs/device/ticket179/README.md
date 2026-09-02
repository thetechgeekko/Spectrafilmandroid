# Export cache and idle pre-render — device evidence (#179)

Device SM-S948W (Tier A hardware), API 36, unplugged for every sample, release build.
Captured 2026-09-02. Raw capture: [`capture.json`](capture.json); host report:
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

## 4. Gate status — honest reading

The report **fails** the Tier A gate. Every finding is environmental, none is about the
measurements:

```
thermal status 2 during BASE/TIFF16 run 5
BASE/TIFF16 run 5 started at thermal 2 after waiting 300s for 0
BASE/ULTRA_HDR run 5 started at thermal 1 after waiting 300s for 0
battery fell to 28%, below the 50% floor
```

The run started at 41% because the owner asked for it to run without charging first; Tier A
requires ≥ 50%. Two samples in run 5 could not reach thermal 0 within the 300 s the harness
waits. A gate-clean SLO capture therefore still needs one run on a charged, cool device — the
numbers above are unlikely to move, since a throttled sample makes an export *slower*, and
these already sit ~57× inside the target.

*Film modeling powered by spektrafilm (GPLv3).*

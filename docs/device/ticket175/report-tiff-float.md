# Export benchmark capture (BASELINE)

> **Export cache bypassed.** Every sample encoded, so these are ENCODER timings. This capture cannot support an SLO claim, which is a claim about the cache-hit path.

- app `0.9.0` (11), APK `6b9f193663d0fe88...`
- device samsung SM-S948W, API 36 (16), 8 cores
- source `66381a8ed2d1a6ab...` 4080x3060

- grade: neutral for every sample, so the post-engine Oklab pass is skipped - as it is on a default export

| cell | format | n | p50 ms | p95 ms | mean +/- 95% CI | peak PSS MB | peak RSS MB |
|---|---|---:|---:|---:|---|---:|---:|
| BASE | JPEG_Q95 | 4 | 5741 | 5771 | 5730 +/- 47 | 86 | 1516 |
| BASE | PNG16 | 5 | 5944 | 5983 | 5939 +/- 28 | 144 | 1516 |
| BASE | TIFF16 | 5 | 5854 | 6143 | 5887 +/- 131 | 254 | 1516 |
| BASE | ULTRA_HDR | 5 | 6004 | 6045 | 6001 +/- 31 | 87 | 1516 |

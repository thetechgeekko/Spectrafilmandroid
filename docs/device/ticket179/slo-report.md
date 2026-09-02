# Export benchmark capture (BASELINE)

- app `0.9.0` (11), APK `1fac6a9da5a1f7e4...`
- device samsung SM-S948W, API 36 (16), 8 cores
- source `66381a8ed2d1a6ab...` 4080x3060

- grade: neutral for every sample, so the post-engine Oklab pass is skipped - as it is on a default export

| cell | format | n | p50 ms | p95 ms | mean +/- 95% CI | peak PSS MB | peak RSS MB |
|---|---|---:|---:|---:|---|---:|---:|
| BASE | JPEG_Q95 | 10 | 35 | 67 | 40 +/- 10 | 85 | 1497 |
| BASE | PNG16 | 11 | 124 | 7392 | 878 +/- 1291 | 145 | 1497 |
| BASE | TIFF16 | 11 | 359 | 6402 | 899 +/- 1079 | 253 | 1497 |
| BASE | ULTRA_HDR | 11 | 40 | 6216 | 601 +/- 1100 | 156 | 1497 |

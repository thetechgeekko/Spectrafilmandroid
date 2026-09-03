# Export benchmark capture (BASELINE)

- app `0.9.0` (11), APK `6b9f193663d0fe88...`
- device samsung SM-S948W, API 36 (16), 8 cores
- source `66381a8ed2d1a6ab...` 4080x3060

- grade: neutral for every sample, so the post-engine Oklab pass is skipped - as it is on a default export

| cell | format | n | p50 ms | p95 ms | mean +/- 95% CI | peak PSS MB | peak RSS MB |
|---|---|---:|---:|---:|---|---:|---:|
| BASE | JPEG_Q95 | 10 | 32 | 68 | 40 +/- 9 | 85 | 1347 |
| BASE | PNG16 | 11 | 107 | 6016 | 643 +/- 1053 | 144 | 1347 |
| BASE | TIFF16 | 11 | 342 | 12030 | 1406 +/- 2082 | 252 | 1347 |
| BASE | ULTRA_HDR | 11 | 51 | 6458 | 632 +/- 1142 | 156 | 1347 |

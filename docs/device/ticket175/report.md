# Export benchmark capture (SMOKE)

- app `0.9.0` (11), APK `e117db8fd3c9a280...`
- device samsung SM-S948W, API 36 (16), 8 cores
- source `66381a8ed2d1a6ab...` 4080x3060

- grade: neutral for every sample, so the post-engine Oklab pass is skipped - as it is on a default export

| cell | format | n | p50 ms | p95 ms | mean +/- 95% CI | peak PSS MB | peak RSS MB |
|---|---|---:|---:|---:|---|---:|---:|
| BASE | JPEG_Q95 | 1 | 5937 | 5937 | n/a | 86 | 1293 |
| BASE | PNG16 | 1 | 6087 | 6087 | n/a | 145 | 1348 |
| BASE | TIFF16 | 1 | 6907 | 6907 | n/a | 255 | 1407 |
| BASE | ULTRA_HDR | 1 | 6390 | 6390 | n/a | 88 | 1516 |
| HEAVY | JPEG_Q95 | 1 | 14612 | 14612 | n/a | 89 | 1685 |
| HEAVY | PNG16 | 1 | 15439 | 15439 | n/a | 193 | 1686 |
| HEAVY | TIFF16 | 1 | 16628 | 16628 | n/a | 256 | 1790 |
| HEAVY | ULTRA_HDR | 1 | 16970 | 16970 | n/a | 90 | 1851 |

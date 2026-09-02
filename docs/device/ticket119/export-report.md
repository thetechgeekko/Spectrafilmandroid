# Export benchmark capture (BASELINE)

- app `0.9.0` (11), APK `e86f327fa33eff46...`
- device samsung SM-S948W, API 36 (16), 8 cores
- source `66381a8ed2d1a6ab...` 4080x3060

- grade: neutral for every sample, so the post-engine Oklab pass is skipped - as it is on a default export

| cell | format | n | p50 ms | p95 ms | mean +/- 95% CI | peak PSS MB | peak RSS MB |
|---|---|---:|---:|---:|---|---:|---:|
| BASE | JPEG_Q95 | 4 | 6052 | 6842 | 6259 +/- 520 | 87 | 1853 |
| BASE | PNG16 | 5 | 7802 | 8884 | 7847 +/- 662 | 147 | 1853 |
| BASE | TIFF16 | 5 | 7108 | 7671 | 7050 +/- 481 | 255 | 1853 |
| HEAVY | JPEG_Q95 | 5 | 14465 | 15839 | 14364 +/- 1075 | 89 | 1853 |
| HEAVY | PNG16 | 5 | 18606 | 21284 | 18859 +/- 1777 | 200 | 1853 |
| HEAVY | TIFF16 | 5 | 15732 | 17885 | 15716 +/- 1429 | 255 | 1853 |
| PRINT_GRAIN | PNG16 | 5 | 14963 | 16424 | 14667 +/- 1376 | 201 | 1853 |
| PRINT_GRAIN | TIFF16 | 5 | 11986 | 13283 | 12026 +/- 991 | 255 | 1853 |
| SCAN_CLEAN | PNG16 | 5 | 6044 | 6460 | 5914 +/- 441 | 145 | 1853 |
| SCAN_CLEAN | TIFF16 | 5 | 5585 | 5850 | 5384 +/- 396 | 255 | 1853 |
| SCAN_GRAIN | PNG16 | 5 | 12241 | 13396 | 11930 +/- 1005 | 204 | 1853 |
| SCAN_GRAIN | TIFF16 | 5 | 9862 | 10624 | 9738 +/- 768 | 255 | 1853 |

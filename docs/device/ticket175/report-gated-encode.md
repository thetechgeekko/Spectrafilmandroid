# Export benchmark capture (BASELINE)

> **Export cache bypassed.** Every sample encoded, so these are ENCODER timings. This capture cannot support an SLO claim, which is a claim about the cache-hit path.

- app `0.9.0` (11), APK `e117db8fd3c9a280...`
- device samsung SM-S948W, API 36 (16), 8 cores
- source `66381a8ed2d1a6ab...` 4080x3060

- grade: neutral for every sample, so the post-engine Oklab pass is skipped - as it is on a default export

| cell | format | n | p50 ms | p95 ms | mean +/- 95% CI | peak PSS MB | peak RSS MB |
|---|---|---:|---:|---:|---|---:|---:|
| BASE | JPEG_Q95 | 4 | 5698 | 5850 | 5734 +/- 101 | 86 | 1817 |
| BASE | PNG16 | 5 | 5908 | 6210 | 5925 +/- 155 | 144 | 1817 |
| BASE | TIFF16 | 5 | 6502 | 6648 | 6501 +/- 85 | 254 | 1817 |
| BASE | ULTRA_HDR | 5 | 6029 | 6124 | 5981 +/- 126 | 87 | 1817 |
| HEAVY | JPEG_Q95 | 5 | 13153 | 13430 | 13201 +/- 115 | 88 | 1817 |
| HEAVY | PNG16 | 5 | 14372 | 14601 | 14276 +/- 278 | 191 | 1817 |
| HEAVY | TIFF16 | 5 | 14171 | 20783 | 15509 +/- 2586 | 254 | 1817 |
| HEAVY | ULTRA_HDR | 5 | 13789 | 14881 | 14018 +/- 475 | 88 | 1817 |

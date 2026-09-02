# Preview-latency capture (#119)

- app `e86f327fa33eff46...` v0.9.0 (11)
- device SM-S948W sdk 36 `samsung/m3qcsx/m3q:16/BP4A.251205.006/S948WVLS4AZG3_OYV4AZG3:user/release-keys`
- thermal 0, battery 68%, plugged 0, cpuset `/foreground`
- decode edge 16384 px, preview 640 px, source `66381a8ed2d1a6ab...`

| route | n | p50 ms | p95 ms | mean +/- 95% CI | engine p50 | bitmap p50 |
|---|---|---|---|---|---|---|
| print | 15 | 106 | 122 | 107 +/- 4 | 101 | 3 |
| scan | 15 | 77 | 85 | 77 +/- 2 | 74 | 3 |

Settle = `SpektraEngine.simulatePreview` on an already-decoded source plus the
ARGB bitmap the editor draws, which is the pair a slider drag repeats; the
grade uses the preset's own values, so a neutral preset skips it. Compose
layout/present cost is additive UI overhead outside this number.

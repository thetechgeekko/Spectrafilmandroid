# Evidence manifest — issue #119 canonical export baseline

Captured 2026-09-02 from source commit `1c7a63e`, app APK
`e86f327fa33eff46cbdfbb5e3afb7805a78918a06f3615234959094236b1f42d`, on SM-S948W
(Android 16 / API 36), unplugged, thermal status 0 on every sample.

Read the numbers in [docs/DEVICE_EXPORT_BASELINE.md](../../DEVICE_EXPORT_BASELINE.md).

| file | sha256 | bytes |
|---|---|---:|
| `capture.json` | `81bdf9cc5b44418d5d1caf44bf39e1838dfd39bd885f7b24f66178ec8b025e16` | 87596 |
| `export-report.md` | `a961c84af8654ea8f1df1d9e03b44d1cd3d08f69a6ccfd919a1a110490b13d26` | 1260 |
| `preview.json` | `959f90392b01f0688fe8bd0e47893d87b7efe723ea74673c83f7385600b17d18` | 2393 |
| `preview-report.md` | `90f96740c40894c6bc48f5e272eb3601d9c64d08e48e4feb636881f0dd81309b` | 801 |
| `stage-timings.log` | `edeb60611ff86afb4d4072ffe71b30151d443ad129c34cb245da666384d0e874` | 5576 |

## Not committed

The Perfetto system trace is 19 MB and would bloat the repository, so it is recorded
by digest instead of stored. Regenerate it with the perfetto leg of the run:

| artifact | sha256 | bytes |
|---|---|---:|
| `t119.pftrace` | `f59b8c57bb3844b74e9e202380c4630d43de64b3c30673e992098fc79e5db616` | 19637424 |

```bash
adb shell perfetto -o /data/misc/perfetto-traces/t119.pftrace -t 120s -b 32mb sched freq idle
```

*Film modeling powered by spektrafilm (GPLv3).*

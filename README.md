# ai-solves-hutter

An attempt at the [Hutter Prize](http://prize.hutter1.net/): losslessly compress
`enwik9` (the first 10^9 bytes of an English Wikipedia dump) as small as possible.

## The target

| | Bytes | Bits/char |
|---|---|---|
| Current record (Vladimir Ivanov, fx2-cmix-T, 24 Jul 2026) | 100,424,672 | 0.803 |
| Needed to claim a prize (1% better) | < 99,420,425 | < 0.795 |
| Shannon's human estimate (~0.6 bpc) | ~75,000,000 | 0.6 |

Score: `S = size(compressor) + size(self-extracting archive)`. When the
compressor and decompressor are the same program, the FAQ counts it once. The
award is `500,000 EUR x (1 - S/L)`, where `L` is the current record.

## Rules that shape the design

- One CPU core, no GPU.
- Under 10 GB RAM and 100 GB disk.
- About 50 hours each for compression and decompression on the test machine (a 2.7 GHz i7 class core).
- The decompressor gets no outside input. Any dictionary or model weights count toward `S`.
- Source code must be documented and open source.

## Results so far

Full log: [`results/results.csv`](results/results.csv). Every row was round-trip verified byte for byte.

| Solution | Input | Archive bytes | bpc | Compress | Decompress | Peak RAM |
|---|---|---|---|---|---|---|
| [cm1](solutions/cm1) | enwik7 (10 MB) | 2,449,272 | 1.959 | 25 s | 20 s | 1.7 GB |
| [cm1](solutions/cm1) | enwik8 (100 MB) | 22,075,602 | 1.766 | 237 s | 179 s | 1.9 GB |

Times are from an AMD Ryzen 5 PRO 7540U laptop under WSL, pinned to one core.

## Layout

| Path | What |
|---|---|
| `data/` | `enwik9` plus `enwik8`/`enwik7` slices (gitignored, fetched by script) |
| `solutions/<name>/` | One compressor per folder, each with its own `build.sh` and README |
| `bench/fetch.sh` | Downloads enwik9 and cuts the slices |
| `bench/run.sh` | Builds, compresses, decompresses, verifies and logs |
| `results/results.csv` | One row per run: date, commit, sizes, bpc, time, peak RAM, verified |

## Running

Everything runs on Linux, because the contest takes Linux binaries. On Windows, use WSL:

```bash
bench/fetch.sh                      # ~300 MB download, unpacks to 1 GB
bench/run.sh cm1                    # enwik7, about a minute
bench/run.sh cm1 enwik9             # the real thing, about 1.5 hours for cm1
```

`run.sh` applies the contest limits rather than just measuring them. It pins
the process to one core with `taskset`, caps the address space at 10 GB with
`ulimit -v`, and records wall time and peak resident memory with
`/usr/bin/time -v`. That is close enough for development, so no VM is needed.
A run that fails verification still gets logged, with `verified=NO`.

From Git Bash, prefix `wsl` calls with `MSYS_NO_PATHCONV=1`, otherwise
`/mnt/c/...` paths get rewritten to Windows paths.

## License

[Unlicense](LICENSE) (public domain), the contest's preferred license.

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

## Leaderboard

The official enwik9 records, with this repo's entries (written by **Claude Opus 5.5**)
in bold. The comparable number is bits per character (bpc): the total size
(compressor plus archive) x 8, divided by the input length. Lower is better.

| Entry | Program | Date | Input | Total bytes | bpc | vs record |
|---|---|---|---|---|---|---|
| Vladimir Ivanov | fx2-cmix-T | 2026-07-24 | enwik9 | 100,424,672 | 0.803 | record |
| David Freelan | cmix-obias | 2026-07-19 | enwik9 | 108,521,870 | 0.868 | +8.1% |
| Ibrahim Marcouch & Kaido Orav | cmix-lex | 2026-06-26 | enwik9 | 109,671,639 | 0.877 | +9.2% |
| Kaido Orav & Byron Knoll | fx2-cmix | 2024-09-03 | enwik9 | 110,793,128 | 0.886 | +10.3% |
| Kaido Orav | fx-cmix | 2024-02-02 | enwik9 | 112,578,322 | 0.901 | +12.1% |
| Saurabh Kumar | fast cmix | 2023-07-16 | enwik9 | 114,156,155 | 0.913 | +13.7% |
| Artemiy Margaritov | starlit | 2021-05-31 | enwik9 | 115,352,938 | 0.923 | +14.9% |
| Alexander Rhatushnyak | phda9 (2020 baseline) | 2019-07-04 | enwik9 | 116,673,681 | 0.933 | +16.2% |
| **Claude Opus 5.5** | **[cm1](solutions/cm1)** | **2026-10-07** | **enwik9** | **run in progress** | | |
| **Claude Opus 5.5** | **[cm1](solutions/cm1)** | **2026-10-07** | **enwik8** | **22,094,170** | **1.767** | **n/a (enwik8)** |
| **Claude Opus 5.5** | **[cm1](solutions/cm1)** | **2026-10-07** | **enwik7** | **2,467,840** | **1.974** | **n/a (enwik7)** |

"vs record" is `S / record - 1`: how much bigger an entry is than
the record. It is only defined on enwik9. bpc drops as the input grows,
because the model has seen more text, so the enwik8 and enwik7 rows overstate
the gap. Official figures come from [prize.hutter1.net](http://prize.hutter1.net/).

Run details for this repo's entries are in [`results/results.csv`](results/results.csv).
Every row there was round-trip verified byte for byte. cm1 took 237 s to
compress enwik8 and 179 s to decompress it, at 1.9 GB peak RAM, on an AMD
Ryzen 5 PRO 7540U laptop under WSL, pinned to one core.

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

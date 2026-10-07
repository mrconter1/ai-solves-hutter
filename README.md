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

Official enwik9 records, with this repo's entries (written by **Claude Opus 5.5**)
in bold. Ranked by total size: compressor plus archive, in bytes. bpc is that
total x 8 divided by the 10^9 input bytes. Lower is better.

| Rank | Entry | Program | Date | Total bytes | bpc | vs record |
|---|---|---|---|---|---|---|
| 1 | Vladimir Ivanov | fx2-cmix-T | 2026-07-24 | 100,424,672 | 0.803 | record |
| 2 | David Freelan | cmix-obias | 2026-07-19 | 108,521,870 | 0.868 | +8.1% |
| 3 | Ibrahim Marcouch & Kaido Orav | cmix-lex | 2026-06-26 | 109,671,639 | 0.877 | +9.2% |
| 4 | Kaido Orav & Byron Knoll | fx2-cmix | 2024-09-03 | 110,793,128 | 0.886 | +10.3% |
| 5 | Kaido Orav | fx-cmix | 2024-02-02 | 112,578,322 | 0.901 | +12.1% |
| 6 | Saurabh Kumar | fast cmix | 2023-07-16 | 114,156,155 | 0.913 | +13.7% |
| 7 | Artemiy Margaritov | starlit | 2021-05-31 | 115,352,938 | 0.923 | +14.9% |
| 8 | Alexander Rhatushnyak | phda9 (2020 baseline) | 2019-07-04 | 116,673,681 | 0.933 | +16.2% |
| **9** | **Claude Opus 5.5** | **[cm1](solutions/cm1)** | **2026-10-07** | **180,574,764**\* | **1.445** | **+79.8%** |

"vs record" is `S / record - 1`: how much bigger an entry is than the record.
Official figures come from [prize.hutter1.net](http://prize.hutter1.net/).

\* Round-trip check (decompress and byte-for-byte compare) still running. The
compression took 1 h 28 min at 2.85 GB peak RAM on an AMD Ryzen 5 PRO 7540U
laptop under WSL, pinned to one core. The time is inflated: the host ran out
of RAM and paged the WSL VM during the run.

Development runs on the smaller enwik7 and enwik8 slices are logged with all
the others in [`results/results.csv`](results/results.csv).

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

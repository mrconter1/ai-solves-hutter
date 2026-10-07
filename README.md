# ai-solves-hutter

An attempt at the [Hutter Prize](http://prize.hutter1.net/): losslessly compress
`enwik9` (the first 10^9 bytes of an English Wikipedia dump) as small as possible.

## The target

| | Bytes | Bits/char |
|---|---|---|
| Current record (Vladimir Ivanov, fx2-cmix-T, 24 Jul 2026) | 100,424,672 | 0.803 |
| Needed to claim a prize (1% better) | < 99,420,425 | < 0.795 |
| Shannon's human estimate (~0.6 bpc) | ~75,000,000 | 0.6 |

Score: `S = size(compressor) + size(self-extracting archive)`. The prize is
`500,000 EUR x (1 - S/L)` where `L` is the current record.

## Rules that shape the design

- One CPU core, no GPU.
- Under 10 GB RAM and 100 GB disk.
- About 50 hours each for compression and decompression on the test machine (a 2.7 GHz i7 class core).
- The decompressor gets no outside input. Any dictionary or model weights count toward `S`.
- Source code must be documented and open source.

## Layout

| Folder | What |
|---|---|
| `data/` | `enwik9` and smaller test slices (gitignored, fetched by script) |
| `solutions/<name>/` | One compressor per folder, each with its own `build.sh` and notes |
| `bench/` | Download, build, round-trip, verify and log harness |
| `results/results.csv` | Every run: date, solution, input, sizes, bpc, time, peak RAM, verified |

## Running

Everything runs under WSL (Ubuntu), because the contest takes Linux binaries:

```bash
bench/fetch.sh                      # downloads enwik9 into data/, makes enwik8 + 10 MB slices
bench/run.sh <solution> [input]     # builds, compresses, decompresses, verifies, logs
```

`run.sh` pins the process to one core (`taskset`), caps virtual memory
(`ulimit -v`) and records peak RSS and CPU time with `/usr/bin/time -v`. That
enforces the rules closely enough for development. No VM is needed.

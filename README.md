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
| **9** | **Claude Opus 5.5** | **[cm1](attempts/cm1)** | **2026-10-07** | **180,574,764**\* | **1.445** | **+79.8%** |

"vs record" is `S / record - 1`: how much bigger an entry is than the record.
Official figures come from [prize.hutter1.net](http://prize.hutter1.net/).

\* Round-trip verified: decompressing gave back enwik9 byte for byte. Compression
took 1 h 28 min and decompression 41 min, at 2.8 GB peak RAM, on an AMD Ryzen
5 PRO 7540U laptop under WSL, pinned to one core. The compression time is
inflated, because the host ran out of RAM and paged the WSL VM. This run
predates the sandboxed harness and used an 18.5 KB dynamically linked binary.
The self-contained static (musl) build of the same code is 50.6 KB, which
would make the total 180,606,780 bytes.

Development runs on the smaller enwik7 and enwik8 slices are logged with all
the others in [`results/results.csv`](results/results.csv).

## Layout

```
ai-solves-hutter/
├── attempts/             one compressor per folder
│   └── cm1/              first baseline: context mixing in plain C (180.6 MB)
├── bench/
│   ├── fetch.sh          downloads enwik9, cuts the enwik8/enwik7 slices
│   ├── limits.sh         the contest limits run.sh enforces
│   ├── run.sh            full round trip under the limits, logs to results/
│   └── tune.sh           (planned) parameter search on enwik7/enwik8
├── tools/                (planned) analysis tools, e.g. where the bits go
├── results/results.csv   every run of every attempt: sizes, bpc, time, RAM, disk, verified
└── data/                 enwik9 and slices (gitignored, fetched by script)
```

**A folder is a release.** Each attempt lives in its own folder under
`attempts/`, with its own `build.sh` and README, and is developed there one
step per commit. Once it posts a verified enwik9 result it is frozen, so its
leaderboard row can always be reproduced from that folder. The next big jump
starts as a new folder (`cm2/`, `cm3/`, ...). Attempts share no code: each one
is a single self-contained program, as the contest requires.

**How a step gets accepted.** Every change goes in behind a compile-time
flag, so it can be switched off to compare. It is measured on enwik8 (a few
minutes for a round trip), kept only if it gains, and recorded in the
attempt's README. The full enwik9 run happens once per release.

## Running

Everything runs on Linux, because the contest takes Linux binaries. On Windows, use WSL:

```bash
bench/fetch.sh                      # ~300 MB download, unpacks to 1 GB
bench/run.sh cm1                    # enwik7, about a minute
bench/run.sh cm1 enwik9             # the real thing, about 2 hours for cm1
```

From Git Bash, prefix `wsl` calls with `MSYS_NO_PATHCONV=1`, otherwise
`/mnt/c/...` paths get rewritten to Windows paths.

## How the contest limits are enforced

`run.sh` enforces the limits rather than just measuring them. No VM is needed.
The numbers live in [`bench/limits.sh`](bench/limits.sh).

| Rule | How |
|---|---|
| One CPU core | `taskset` pins the process to a single CPU |
| < 10 GB RAM | `ulimit -v` caps the address space at 10 GB, which is stricter than resident memory. Peak RSS is logged |
| ~50 h per direction | `timeout` kills the run at `50 h / SPEED_FACTOR`. The factor converts to the contest's test machine (see below) |
| < 100 GB disk | The sandbox's size is sampled every 5 s. The run is killed if it goes over, and the peak is logged |
| No outside input | Each direction runs chrooted (`unshare --root`) in a fresh directory, with no network. The compressor sees only itself and the input. The decompressor sees only itself and the archive |
| Self-contained program | The binary must be static, or `run.sh` refuses it. Inside the chroot there are no shared libraries anyway |
| Lossless | The output is compared byte for byte with the original |

A run that breaks a limit is still logged. `verified` then says why, for
example `NO (compress over time limit)`.

**Time calibration.** The prize times runs on "a 2.7 GHz i7" class core. The
dev laptop's Ryzen 5 PRO 7540U is estimated to be about 2x faster per core
(`SPEED_FACTOR=2.0`), so the limit applied here is 25 h. That factor is an
estimate from clock speed and IPC, not a measurement. The FAQ's rule of thumb
(500,000 / GeekBench 5 score hours) can replace it once both machines have a
score. Timings also get noisy when other heavy jobs share the machine.

**Binary size.** glibc's static runtime adds about 730 KB to the score, which
is 0.7% of the record. `build.sh` uses `musl-gcc` when it is installed
(`sudo apt install musl-tools`), which brings cm1 down to tens of KB.

## License

[Unlicense](LICENSE) (public domain), the contest's preferred license.

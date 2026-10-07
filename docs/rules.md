# Rules and how they're enforced

## The contest

Losslessly compress `enwik9`, the first 10^9 bytes of an English Wikipedia
dump. Details: [prize.hutter1.net](http://prize.hutter1.net/).

| | Bytes | Bits/char |
|---|---|---|
| Current record (Vladimir Ivanov, fx2-cmix-T, 24 Jul 2026) | 100,424,672 | 0.803 |
| Needed to claim a prize (1% better) | < 99,420,425 | < 0.795 |
| Shannon's human estimate (~0.6 bpc) | ~75,000,000 | 0.6 |

Score: `S = size(compressor) + size(self-extracting archive)`. When the
compressor and decompressor are the same program, the FAQ counts it once. The
award is `500,000 EUR x (1 - S/L)`, where `L` is the current record.

The limits that shape every design:

- One CPU core, no GPU.
- Under 10 GB RAM and 100 GB disk.
- About 50 hours each for compression and decompression on the test machine
  (a 2.7 GHz i7 class core).
- The decompressor gets no outside input. Any dictionary or model weights
  count toward `S`.
- Source code must be documented and open source.

## How `bench/run.sh` enforces them

It enforces the limits rather than just measuring them. The numbers live in
[`bench/limits.sh`](../bench/limits.sh).

| Rule | How |
|---|---|
| One CPU core | `taskset` pins the process to a single CPU |
| < 10 GB RAM | `ulimit -v` caps the address space at 10 GB, which is stricter than resident memory. Peak RSS is logged |
| ~50 h per direction | `timeout` kills the run at `50 h / SPEED_FACTOR` (see below) |
| < 100 GB disk | The sandbox's size is sampled every 5 s. The run is killed if it goes over, and the peak is logged |
| No outside input | Each direction runs chrooted (`unshare --root`) in a fresh directory with no network. The compressor sees only itself and the input; the decompressor only itself and the archive |
| Self-contained program | The binary must be static, or `run.sh` refuses it |
| Lossless | The output is compared byte for byte with the original |

A run that breaks a limit is still logged; `verified` then says why, for
example `NO (compress over time limit)`.

**Time calibration.** cm2 on enwik7 took 14 s per direction on a Google Cloud
n2d (AMD EPYC 7B13) and 29 s on the dev laptop under WSL. We assume the
contest machine is about as fast as the n2d, so `SPEED_FACTOR=1.0` and the
limit is 50 h. That assumption is unmeasured. A slower machine only makes the
limit more generous than the contest's, which is why release timings come
from the cloud.

**Binary size.** glibc's static runtime adds about 730 KB to the score (0.7%
of the record). `build.sh` uses `musl-gcc` when it is installed, which keeps
the binary at tens of KB. The exact size depends on the toolchain, so release
scores come from the cloud build.

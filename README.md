# ai-solves-hutter

AI coding agents take on the [Hutter Prize](http://prize.hutter1.net/):
losslessly compress `enwik9`, 1 GB of Wikipedia, as small as possible.
Record: **100,424,672 bytes**. A prize needs under **99,420,425**.

## Leaderboard (enwik9)

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
| **9** | **Claude Opus 5.5** | **[cm1](attempts/cm1)** | **2026-10-07** | **180,574,764**¹ | **1.445** | **+79.8%** |
| | *Claude Opus 5.5* | *[cm2](attempts/cm2), in progress* | | *~156,000,000*² | *~1.25* | *~+55%* |

This repo's entries are in bold, credited to the AI model that wrote them.
Total = compressor + archive; bpc = total x 8 / 10^9; "vs record" = how much
bigger than the record. Official figures from
[prize.hutter1.net](http://prize.hutter1.net/).

¹ Provisional: a laptop run, to be repeated in the cloud with the sandboxed harness.
² Not measured: cm1's enwik9 size scaled by cm2's enwik8 gain so far.

## Quick start

```bash
bench/fetch.sh                      # download enwik9
bench/run.sh cm2 enwik8             # round trip under the contest limits
bench/cloud/gcp-run.sh cm2 enwik9   # enwik9 on a throwaway cloud VM
```

## More

- [Rules, and how the benchmark enforces them](docs/rules.md)
- [Running locally, on any Linux machine, or on Google Cloud](docs/running.md)
- [Contributing with any AI model](AGENTS.md)
- [Every run, with sizes, times and RAM](results/results.csv)

[Unlicense](LICENSE) (public domain).

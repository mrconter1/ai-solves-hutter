# Running

Everything runs on Linux, because the contest takes Linux binaries.

## Locally

```bash
bench/fetch.sh                      # ~300 MB download, unpacks to 1 GB, cuts enwik8/enwik7
bench/run.sh cm2                    # enwik7, about a minute
bench/run.sh cm2 enwik8             # a few minutes; how steps get accepted
CFLAGS="-DTABLE_BITS=26" NOTE="release tables" bench/run.sh cm2 enwik8
```

Each run appends one row to [`results/results.csv`](../results/results.csv).

On Windows, use WSL. From Git Bash, prefix `wsl` calls with
`MSYS_NO_PATHCONV=1`, otherwise `/mnt/c/...` paths get rewritten. Don't run
enwik9 on a laptop that's doing other things: when the host runs short of
RAM, the WSL VM gets paged and a run can slow down 10 to 50x.

## On any clean Linux machine

```bash
git clone https://github.com/mrconter1/ai-solves-hutter && cd ai-solves-hutter
sudo bench/remote-run.sh cm2 enwik9 out/      # toolchain, data, run, verify
cat out/STATUS out/result.csv                 # OK + the result row
```

Works on any fresh Debian or Ubuntu machine with 16 GB of RAM: a VM on any
cloud, a rented server or your own computer. Run it as root, because the
sandbox uses `unshare --root`, which Ubuntu 24.04 restricts for normal users.

## On Google Cloud

```bash
bench/cloud/gcp-run.sh cm2 enwik9             # starts a throwaway VM, returns at once
bench/cloud/gcp-fetch.sh                      # list runs and their status
bench/cloud/gcp-fetch.sh <run-id>             # log tail; appends the row when done
```

The VM gets the committed tree (`git archive HEAD`), runs
`bench/remote-run.sh`, uploads the results to a bucket and deletes itself.
`--max-run-duration` deletes it anyway if anything hangs, so it can't keep
billing. The default machine is `n2d-standard-4` (AMD EPYC, 16 GB, about
$0.19/h): an enwik8 run costs a few cents, an enwik9 round trip about $1.
Settings: `GCP_PROJECT`, `GCP_ZONE`, `GCP_BUCKET`, `MAX_HOURS`, `NOTE`,
`CFLAGS`.

### Experiments: many variants on one VM

```bash
cat > work/variants.txt <<'V'
base
lr2   -DMIXER_LR=2
mix2  -DUSE_MIX2=1 -DMIX2_LR=3
V
bench/cloud/gcp-batch.sh cm2 enwik7 work/variants.txt   # one n2d-standard-8 VM
bench/cloud/gcp-fetch.sh <run-id>                       # results table when done
```

For parameter sweeps, `bench/tune.sh` writes the variants file: one variant
per value, around the current defaults (or around `BASE_FLAGS`):

```bash
bench/tune.sh MIXER_LR=3,5 APM_RATE=6,8 > work/variants.txt
```

`bench/batch-run.sh` builds every variant (`<name> <CFLAGS...>` per line) and
round-trips it, as many in parallel as cores and RAM allow, then writes a
table sorted by size. Every batch first runs `tools/edgecases.sh` on the
default build (the `edgecases` row). It's for choosing between ideas, not for
leaderboard numbers: there is no sandbox, but every round trip is verified.
A batch of enwik7 variants takes about 5 minutes and a few cents.

An adapter for another provider only needs to do the same three things: get
the tree onto a VM, run `bench/remote-run.sh`, and bring `out/` back.

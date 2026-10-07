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

An adapter for another provider only needs to do the same three things: get
the tree onto a VM, run `bench/remote-run.sh`, and bring `out/` back.

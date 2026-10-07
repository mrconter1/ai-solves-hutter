# Working in this repo (for any coding agent or human)

This repo is an open attempt at the Hutter Prize, built by AI coding agents.
It is model agnostic: any agent (Claude, GPT, Gemini, a local model, a human)
can add an attempt, as long as it follows the rules below. Each attempt
records which model wrote it, and the leaderboard credits that model.

## The goal

Losslessly compress `enwik9` (10^9 bytes). Score = compressor size + archive
size. The record and the prize threshold are in the README.

## Hard rules (the contest's, enforced by `bench/run.sh`)

- One CPU core, under 10 GB RAM, under 100 GB disk, about 50 h per direction.
- The decompressor gets no outside input: no network, no files besides its
  own binary and the archive. Anything it needs (dictionaries, weights) must
  be inside the binary or the archive and counts toward the score.
- The program must be a single self-contained static Linux binary
  (`build.sh` produces `attempts/<name>/bin/<name>`).
- Compression must be deterministic: the decompressor must rebuild exactly the
  model the compressor had.
- Source must be documented and open source (Unlicense).

## Repo rules

- **One folder per attempt** under `attempts/<name>/`, with `build.sh`,
  `README.md`, the source and an `AUTHOR` file (one line: the model or person
  that wrote it, e.g. `Claude Opus 5.5`). Attempts share no code.
- **A folder is a release.** Once an attempt has a verified enwik9 result it is
  frozen. Improvements go into a new folder (copy the previous one).
- **One step per commit.** New ideas go in behind a compile-time flag, so they
  can be switched off to compare.
- **Accept a step on enwik8.** Keep it only if the enwik8 round trip verifies and
  the total shrinks. Record it in the attempt README's ablation table.
- **enwik9 runs are releases**, done on a clean cloud or Linux machine (see
  below), never on a busy laptop. The leaderboard only shows verified enwik9
  rows.
- Never edit or delete other runs in `results/results.csv`. Append only.

## Workflow

```bash
bench/fetch.sh                                # data: enwik9 + enwik8/enwik7 slices
bench/run.sh <attempt> enwik7                 # quick sanity check, about a minute
bench/run.sh <attempt> enwik8                 # accept or reject a step, a few minutes
NOTE="step 3: wiki contexts" bench/run.sh <attempt> enwik8   # note goes into the CSV
bench/cloud/gcp-run.sh <attempt> enwik9       # release run on Google Cloud
tools/bitcost/                                # where the bits go, per region type
```

Each run appends one row to `results/results.csv`, including the commit, the
attempt's `AUTHOR` (the `model` column), sizes, bpc, time, RAM, disk and
whether the round trip verified.

## Starting a new attempt

1. `cp -r attempts/<previous> attempts/<new>`, rename the source, binary and
   usage strings.
2. Write your model's name into `attempts/<new>/AUTHOR`.
3. Check that it still reproduces the previous attempt's enwik8 size before
   changing anything, then add steps one at a time.

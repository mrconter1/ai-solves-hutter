#!/usr/bin/env bash
# Runs a batch of build variants (bench/batch-run.sh) on ONE Google Cloud VM,
# so experiments don't load the laptop. Returns at once.
#
#   bench/cloud/gcp-batch.sh <attempt> <input> <variants-file> [machine=n2d-standard-8]
#   bench/cloud/gcp-fetch.sh <run-id>      # status; prints results.tsv when done
#
# Uses the committed tree of HEAD, like gcp-run.sh, and the same cost guard
# (MAX_VMS, default 2). Default MAX_HOURS is 2.
set -euo pipefail
cd "$(dirname "$0")/../.."
attempt=${1:?usage: gcp-batch.sh <attempt> <input> <variants-file> [machine]}
input=${2:?input}
variants=${3:?variants file}
machine=${4:-n2d-standard-8}
[ -f "$variants" ] || { echo "no such file: $variants" >&2; exit 1; }
export MAX_HOURS=${MAX_HOURS:-2}
export BATCH_VARIANTS="$variants"
exec bash bench/cloud/gcp-run.sh "$attempt" "$input" "$machine"

#!/usr/bin/env bash
# Runs one benchmark on a fresh Linux machine: any cloud VM, a rented server or
# your own box. Provider adapters (bench/cloud/*) only get the code onto the
# machine, call this, and collect <outdir> afterwards.
#
#   sudo bench/remote-run.sh <attempt> [input=enwik9] [outdir=out]
#
# Installs the toolchain if it can (Debian/Ubuntu, as root), fetches the data
# unless data/enwik9.zip or data/enwik9 is already there, runs bench/run.sh
# under the contest limits and writes to <outdir>:
#   result.csv   the result row      STATUS   OK or FAILED: <reason>
#   run.log      full output         *.time   /usr/bin/time reports
#
# Run as root, or make sure unprivileged user namespaces are allowed: the
# sandbox in run.sh uses `unshare --root`, which Ubuntu 24.04 restricts.
# Pass COMMIT=<hash> when the tree has no .git (an exported archive).
# CFLAGS (extra build flags) and NOTE pass through to bench/run.sh.
# With BATCH_VARIANTS=<file> it runs bench/batch-run.sh instead (experiments).
set -uo pipefail
cd "$(dirname "$0")/.."
attempt=${1:?usage: remote-run.sh <attempt> [input] [outdir]}
input=${2:-enwik9}
out=${3:-out}
mkdir -p "$out"
out=$(cd "$out" && pwd)
exec > >(tee -a "$out/run.log") 2>&1

status() { echo "$1" > "$out/STATUS"; echo "== $1"; }
status running
echo "== $(date -u +%FT%TZ) $attempt on $input, commit ${COMMIT:-from git}"

need() { command -v "$1" >/dev/null 2>&1; }
if ! need gcc || ! need musl-gcc || ! [ -x /usr/bin/time ]; then
  if need apt-get && [ "$(id -u)" -eq 0 ]; then
    DEBIAN_FRONTEND=noninteractive apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq build-essential musl-tools time python3 curl >/dev/null
  else
    echo "missing toolchain: need gcc, musl-gcc (optional but smaller binary), /usr/bin/time, python3, curl"
    need gcc && [ -x /usr/bin/time ] || { status "FAILED: missing toolchain"; exit 1; }
  fi
fi

bash bench/fetch.sh || { status "FAILED: data fetch"; exit 1; }

rc=0
if [ -n "${BATCH_VARIANTS:-}" ]; then
  # Experiment batch: many build variants, results table instead of a CSV row.
  bash bench/batch-run.sh "$attempt" "$input" "$BATCH_VARIANTS" "$out" || rc=$?
  if [ "$rc" -eq 0 ]; then status OK; else status "FAILED: batch-run.sh exit $rc"; fi
  exit "$rc"
fi
bash bench/run.sh "$attempt" "$input" || rc=$?
tail -n 1 results/results.csv > "$out/result.csv"
cp work/"$attempt"/*.time "$out/" 2>/dev/null || true
if [ "$rc" -eq 0 ]; then status OK; else status "FAILED: run.sh exit $rc"; fi
exit "$rc"

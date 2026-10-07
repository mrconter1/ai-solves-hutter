#!/usr/bin/env bash
# Runs many build variants of one attempt on one machine, in parallel, and
# writes a results table. For experiments (which variant helps?), not for
# leaderboard numbers: no sandbox, but every round trip is still verified.
#
#   bench/batch-run.sh <attempt> <input> <variants-file> [outdir=out]
#
# Variants file: one per line, "<name> <CFLAGS...>"; blank lines and # comments
# are ignored. A line with just a name builds with no extra flags.
# Parallelism: JOBS, default min(cores, RAM / PER_JOB_GB) with PER_JOB_GB=2.5.
# Output: <outdir>/results.tsv (name, archive bytes, compress s, decompress s,
# peak MB, verified, cflags), sorted by size, after an "edgecases" row for
# tools/edgecases.sh on the default build.
set -uo pipefail
cd "$(dirname "$0")/.."
attempt=${1:?usage: batch-run.sh <attempt> <input> <variants-file> [outdir]}
input=${2:?input}
variants=${3:?variants file}
out=${4:-out}
mkdir -p "$out"; out=$(cd "$out" && pwd)
src="$(pwd)/data/$input"
[ -f "$src" ] || { echo "missing $src, run bench/fetch.sh" >&2; exit 1; }

per_job_gb=${PER_JOB_GB:-2.5}
mem_gb=$(awk '/MemTotal/{print int($2/1048576)}' /proc/meminfo)
jobs=${JOBS:-$(awk -v c="$(nproc)" -v m="$mem_gb" -v p="$per_job_gb" 'BEGIN{j=int(m/p); if (j>c) j=c; if (j<1) j=1; print j}')}
echo "== batch: $attempt on $input, $(grep -cvE '^\s*(#|$)' "$variants") variants, $jobs at a time"

one() {
  local name=$1; shift
  local flags="$*" dir="$BATCH_DIR/$name"
  rm -rf "$dir"; mkdir -p "$dir"
  cp -r "attempts/$ATTEMPT/." "$dir/"
  rm -rf "$dir/bin"
  if ! CFLAGS="$flags" bash "$dir/build.sh" >"$dir/build.log" 2>&1; then
    printf '%s\t0\t0\t0\t0\tBUILD FAILED\t%s\n' "$name" "$flags"; return
  fi
  local bin="$dir/bin/$ATTEMPT"
  /usr/bin/time -f '%e %M' -o "$dir/c.t" "$bin" c "$SRC" "$dir/arc" >/dev/null 2>&1
  /usr/bin/time -f '%e %M' -o "$dir/d.t" "$bin" d "$dir/arc" "$dir/restored" >/dev/null 2>&1
  local ok=NO; cmp -s "$SRC" "$dir/restored" && ok=yes
  local size; size=$(stat -c %s "$dir/arc" 2>/dev/null || echo 0)
  read -r ct cm < "$dir/c.t"; read -r dt dm < "$dir/d.t"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$size" "$ct" "$dt" "$(( (cm > dm ? cm : dm) / 1024 ))" "$ok" "$flags"
  rm -f "$dir/restored" "$dir/arc"
}
export -f one

# Edge cases first, on the default build (tools/edgecases.sh): a row "edgecases"
# with verified=yes/NO, so no experiment batch can skip them.
ec=NO
[ -f tools/edgecases.sh ] && bash tools/edgecases.sh "$attempt" > "$out/edgecases.log" 2>&1 && ec=yes
printf 'edgecases\t0\t0\t0\t0\t%s\t(default build, see edgecases.log)\n' "$ec" > "$out/edgecases.row"
export ATTEMPT="$attempt" SRC="$src" BATCH_DIR="$(pwd)/work/batch"

grep -vE '^\s*(#|$)' "$variants" | xargs -P "$jobs" -L 1 bash -c 'one "$@"' _ > "$out/results.unsorted"
{
  printf 'name\tarchive_bytes\tcompress_s\tdecompress_s\tpeak_mb\tverified\tcflags\n'
  sort -t "$(printf '\t')" -k2,2n "$out/results.unsorted"
} > "$out/results.tsv"
rm -f "$out/results.unsorted" "$out/edgecases.row"
column -t -s "$(printf '\t')" "$out/results.tsv"

#!/usr/bin/env bash
# Builds a solution, compresses an input, decompresses it, verifies the result
# byte for byte and appends one row to results/results.csv.
#
#   bench/run.sh <solution> [input=enwik7]
#
# Contest limits are applied, not just measured:
#   one core      taskset pins the process to a single CPU
#   10 GB RAM     ulimit -v caps the address space
# and /usr/bin/time records CPU time and peak resident memory.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$(pwd)

sol=${1:?usage: bench/run.sh <solution> [input]}
input=${2:-enwik7}
record=100424672          # current enwik9 record (Ivanov, 24 Jul 2026)
mem_kb=$((10 * 1024 * 1024))
core=${CORE:-2}

src="$root/data/$input"
[ -f "$src" ] || { echo "missing $src, run bench/fetch.sh" >&2; exit 1; }

bash "solutions/$sol/build.sh"
bin="$root/solutions/$sol/bin/$sol"
work="$root/work/$sol"
mkdir -p "$work" results
archive="$work/$input.cmp"
restored="$work/$input.out"

limited() {   # limited <timefile> cmd...
  local tf=$1; shift
  ( ulimit -v "$mem_kb"; taskset -c "$core" /usr/bin/time -v -o "$tf" "$@" )
}
stat_field() { grep -F "$2" "$1" | awk -F': ' '{print $2}'; }
seconds() {   # "h:mm:ss" or "m:ss.ss" -> seconds
  awk -F: '{ if (NF==3) print $1*3600+$2*60+$3; else print $1*60+$2 }' <<<"$1"
}

echo "== $sol on $input: compress"
limited "$work/c.time" "$bin" c "$src" "$archive"
echo "== decompress"
limited "$work/d.time" "$bin" d "$archive" "$restored"

if cmp -s "$src" "$restored"; then verified=yes; else verified=NO; fi
rm -f "$restored"

in_bytes=$(stat -c %s "$src")
arc_bytes=$(stat -c %s "$archive")
bin_bytes=$(stat -c %s "$bin")
total=$((arc_bytes + bin_bytes))
bpc=$(awk -v a="$arc_bytes" -v n="$in_bytes" 'BEGIN{printf "%.4f", a*8/n}')
c_s=$(seconds "$(stat_field "$work/c.time" 'Elapsed (wall clock)')")
d_s=$(seconds "$(stat_field "$work/d.time" 'Elapsed (wall clock)')")
c_mb=$(( $(stat_field "$work/c.time" 'Maximum resident') / 1024 ))
d_mb=$(( $(stat_field "$work/d.time" 'Maximum resident') / 1024 ))
commit=$(git rev-parse --short HEAD)$([ -z "$(git status --porcelain -- "solutions/$sol")" ] || echo "-dirty")
cpu=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ *//; s/,/ /g')

# vs record only means something on the contest file
if [ "$input" = enwik9 ]; then
  vs=$(awk -v s="$total" -v l="$record" 'BEGIN{printf "%+.2f%%", (1-s/l)*100}')
else
  vs=$(awk -v a="$arc_bytes" -v n="$in_bytes" -v l="$record" 'BEGIN{printf "est %+.2f%%", (1-(a*1e9/n)/l)*100}')
fi

csv=results/results.csv
[ -f "$csv" ] || echo "date_utc,commit,solution,input,input_bytes,archive_bytes,binary_bytes,total_bytes,bpc,vs_record,compress_s,decompress_s,compress_peak_mb,decompress_peak_mb,verified,cpu" > "$csv"
echo "$(date -u +%Y-%m-%dT%H:%M:%SZ),$commit,$sol,$input,$in_bytes,$arc_bytes,$bin_bytes,$total,$bpc,$vs,$c_s,$d_s,$c_mb,$d_mb,$verified,$cpu" >> "$csv"

printf '\n%-14s %s\n' solution "$sol" input "$input ($in_bytes bytes)" archive "$arc_bytes" binary "$bin_bytes" total "$total" bpc "$bpc" "vs record" "$vs (record 100,424,672 on enwik9)" compress "${c_s}s, ${c_mb} MB peak" decompress "${d_s}s, ${d_mb} MB peak" verified "$verified"
[ "$verified" = yes ]

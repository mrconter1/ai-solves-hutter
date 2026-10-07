#!/usr/bin/env bash
# Builds an attempt, compresses an input, decompresses it, verifies the result
# byte for byte and appends one row to results/results.csv.
#
#   bench/run.sh <attempt> [input=enwik7]
#   NOTE="..." bench/run.sh ...    adds a free-text note to the CSV row
#
# Each direction runs in its own sandbox. The limits come from bench/limits.sh:
#
#   one core         taskset pins the process to one CPU
#   memory           ulimit -v caps the address space (10 GB)
#   time             timeout kills the run at 50 h / SPEED_FACTOR
#   disk             the sandbox's peak size is sampled and must stay under 100 GB
#   no outside input the binary runs chrooted into a fresh directory that holds
#                    only itself and its input, with no network (unshare). The
#                    decompressor gets the binary and the archive, nothing else.
#   self-contained   the binary must be statically linked, or the chroot fails
#
# A run that breaks a limit still gets logged, with the reason in `verified`.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$(pwd)
. bench/limits.sh

sol=${1:?usage: bench/run.sh <attempt> [input]}
input=${2:-enwik7}
record=100424672          # current enwik9 record (Ivanov, 24 Jul 2026)
core=${CORE:-2}
NOTE=${NOTE:-}
mem_kb=$((MEM_LIMIT_GB * 1024 * 1024))
time_limit_s=$(awk -v h="$TIME_LIMIT_H" -v f="$SPEED_FACTOR" 'BEGIN{printf "%d", h*3600/f}')
disk_limit=$((DISK_LIMIT_GB * 1000 * 1000 * 1000))

src="$root/data/$input"
[ -f "$src" ] || { echo "missing $src, run bench/fetch.sh" >&2; exit 1; }

bash "attempts/$sol/build.sh"
bin="$root/attempts/$sol/bin/$sol"
if ldd "$bin" >/dev/null 2>&1; then
  echo "$bin is dynamically linked; the contest needs a self-contained binary" >&2
  exit 1
fi

work="$root/work/$sol"
mkdir -p "$work" results
archive="$work/$input.cmp"

stat_field() { grep -F "$2" "$1" | awk -F': ' '{print $2}'; }
seconds() {   # "h:mm:ss" or "m:ss.ss" -> seconds
  awk -F: '{ if (NF==3) print $1*3600+$2*60+$3; else print $1*60+$2 }' <<<"$1"
}

# sandboxed <dir> <timefile> <diskfile> args...
# Runs /<sol> args... chrooted into <dir>, under every limit. Prints the exit
# status; 124 or 137 means the time limit hit.
sandboxed() {
  local dir=$1 tf=$2 df=$3; shift 3
  (
    ulimit -v "$mem_kb"
    exec taskset -c "$core" /usr/bin/time -v -o "$tf" \
      timeout --signal=KILL "$time_limit_s" \
      unshare -r -n -m -p -f --kill-child --root="$dir" --wd=/ "/$sol" "$@"
  ) &
  local pid=$! peak=0 now
  while kill -0 "$pid" 2>/dev/null; do
    now=$(du -sb "$dir" 2>/dev/null | cut -f1 || echo 0)
    [ "$now" -gt "$peak" ] && peak=$now
    if [ "$peak" -gt "$disk_limit" ]; then pkill -TERM -P "$pid" || true; fi  # timeout forwards TERM, unshare kills the child
    sleep "$DISK_SAMPLE_S"
  done
  local rc=0
  wait "$pid" || rc=$?
  now=$(du -sb "$dir" | cut -f1)
  [ "$now" -gt "$peak" ] && peak=$now
  echo "$peak" > "$df"
  return "$rc"
}

fail=""
note_fail() { fail="${fail:+$fail; }$1"; }
check_rc() {  # check_rc <direction> <rc> <diskfile>
  local peak; peak=$(cat "$3")
  if [ "$peak" -gt "$disk_limit" ]; then note_fail "$1 over disk limit"
  elif [ "$2" -eq 137 ] || [ "$2" -eq 124 ]; then note_fail "$1 over time limit"
  elif [ "$2" -ne 0 ]; then note_fail "$1 exit $2"; fi
}

echo "== $sol on $input: compress (limit $((time_limit_s / 3600)) h here = ${TIME_LIMIT_H} h on the test machine)"
cbox="$work/sandbox-c"
rm -rf "$cbox" && mkdir -p "$cbox"
cp "$bin" "$cbox/$sol"
cp "$src" "$cbox/input"
rc=0; sandboxed "$cbox" "$work/c.time" "$work/c.disk" c input archive || rc=$?
check_rc compress "$rc" "$work/c.disk"
[ -f "$cbox/archive" ] && mv "$cbox/archive" "$archive"
rm -rf "$cbox"

echo "== decompress"
dbox="$work/sandbox-d"
rm -rf "$dbox" && mkdir -p "$dbox"
cp "$bin" "$dbox/$sol"
[ -f "$archive" ] && cp "$archive" "$dbox/archive"
rc=0; sandboxed "$dbox" "$work/d.time" "$work/d.disk" d archive output || rc=$?
check_rc decompress "$rc" "$work/d.disk"

if [ -z "$fail" ] && [ -f "$dbox/output" ] && cmp -s "$src" "$dbox/output"; then
  verified=yes
else
  [ -n "$fail" ] || note_fail "output differs"
  verified="NO ($fail)"
fi
rm -rf "$dbox"

in_bytes=$(stat -c %s "$src")
arc_bytes=$(stat -c %s "$archive" 2>/dev/null || echo 0)
bin_bytes=$(stat -c %s "$bin")
total=$((arc_bytes + bin_bytes))
bpc=$(awk -v t="$total" -v n="$in_bytes" 'BEGIN{printf "%.4f", t*8/n}')
c_s=$(seconds "$(stat_field "$work/c.time" 'Elapsed (wall clock)')")
d_s=$(seconds "$(stat_field "$work/d.time" 'Elapsed (wall clock)')")
c_mb=$(( $(stat_field "$work/c.time" 'Maximum resident') / 1024 ))
d_mb=$(( $(stat_field "$work/d.time" 'Maximum resident') / 1024 ))
disk_mb=$(( $(sort -n "$work/c.disk" "$work/d.disk" | tail -1) / 1000000 ))
# COMMIT can be passed in when running from an exported tree (cloud runs have no .git)
commit=${COMMIT:-$(git rev-parse --short HEAD)$([ -z "$(git status --porcelain -- "attempts/$sol")" ] || echo "-dirty")}
cpu=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ *//; s/,/ /g')

# How much bigger than the record; only meaningful on the contest file.
if [ "$input" = enwik9 ]; then
  vs=$(awk -v s="$total" -v l="$record" 'BEGIN{printf "%+.1f%%", (s/l-1)*100}')
else
  vs=n/a
fi

csv=results/results.csv
[ -f "$csv" ] || echo "date_utc,commit,attempt,input,input_bytes,archive_bytes,binary_bytes,total_bytes,bpc,vs_record,compress_s,decompress_s,compress_peak_mb,decompress_peak_mb,peak_disk_mb,verified,cpu,note" > "$csv"
echo "$(date -u +%Y-%m-%dT%H:%M:%SZ),$commit,$sol,$input,$in_bytes,$arc_bytes,$bin_bytes,$total,$bpc,$vs,$c_s,$d_s,$c_mb,$d_mb,$disk_mb,$verified,$cpu,${NOTE//,/;}" >> "$csv"

printf '\n%-14s %s' attempt "$sol" input "$input ($in_bytes bytes)" archive "$arc_bytes" binary "$bin_bytes" \
  total "$total" bpc "$bpc" "vs record" "$vs" compress "${c_s}s, ${c_mb} MB peak" \
  decompress "${d_s}s, ${d_mb} MB peak" disk "${disk_mb} MB peak" verified "$verified"
echo
[ "$verified" = yes ]

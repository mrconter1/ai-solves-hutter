#!/usr/bin/env bash
# Writes a variants file for one-at-a-time parameter sweeps, ready for
# bench/cloud/gcp-batch.sh (or bench/batch-run.sh on any Linux box).
#
#   bench/tune.sh NAME=v1,v2,... [NAME=...] > work/variants.txt
#   bench/cloud/gcp-batch.sh cm2 enwik8 work/variants.txt
#
# Each NAME must be a compile-time define guarded with #ifndef in the
# attempt's source, so -DNAME=value overrides its default. The output has a
# `base` line (current defaults) plus one line per value, named
# <name>_<value> in lower case. BASE_FLAGS="-DX=1 ..." is added to every line,
# including base, to sweep around a combination instead of the defaults.
set -euo pipefail
[ $# -ge 1 ] || { sed -n '2,11p' "$0" >&2; exit 1; }
base=${BASE_FLAGS:-}
echo "base $base"
for spec in "$@"; do
  name=${spec%%=*}
  values=${spec#*=}
  [ "$name" != "$spec" ] && [ -n "$values" ] || { echo "bad spec: $spec (want NAME=v1,v2)" >&2; exit 1; }
  IFS=, read -r -a vs <<<"$values"
  for v in "${vs[@]}"; do
    echo "$(echo "${name}_${v}" | tr 'A-Z' 'a-z' | tr -c 'a-z0-9_\n' '_') $base -D$name=$v"
  done
done

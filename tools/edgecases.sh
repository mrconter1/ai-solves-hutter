#!/usr/bin/env bash
# Round-trips awkward inputs through an attempt's binary and fails loudly on
# any mismatch. enwik never exercises these, but a real compressor must
# survive them (and preprocessing stages tend to break on exactly these).
#
#   tools/edgecases.sh <attempt>
#
# Runs the binary directly (no sandbox). Needs only a few seconds per case.
set -euo pipefail
cd "$(dirname "$0")/.."
attempt=${1:?usage: tools/edgecases.sh <attempt>}
bash "attempts/$attempt/build.sh"
bin="$PWD/attempts/$attempt/bin/$attempt"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

: > "$tmp/empty"
printf 'A' > "$tmp/one-byte"
head -c 65536 /dev/urandom > "$tmp/random-64k"
head -c 1048576 /dev/zero > "$tmp/zeros-1m"
head -c 300000 /dev/urandom | base64 -w 0 > "$tmp/no-newlines"
printf '\xff\x00\xff\x00\x80\x7f' > "$tmp/high-bytes"
for i in $(seq 1 2000); do printf 'The Quick brown fox &amp; [[Link|text]] {{Tmpl}} %d\n' "$i"; done > "$tmp/wikiish"

# step 4 transform: case forms, broken entities, very long words, letters next to UTF-8
for i in $(seq 1 1500); do
  printf 'THE The the tHe McDonald A I iPhone ABC123 X x &am &amp &amp;&amp; &quot;&lt;&gt; Caf\xc3\xa9 \xc3\x9cber na\xc3\xafve %d\n' "$i"
done > "$tmp/caps-ents"
python3 -c "import sys; w='Ab'*200; sys.stdout.write((' '.join([w, w.upper(), w.lower(), 'x'*300]) + chr(10)) * 200)" > "$tmp/long-words"
# every byte value present: no free bytes, so the transform must switch itself off
python3 -c "import sys; sys.stdout.buffer.write(bytes(range(256)) + b'the quick brown fox ' * 5000)" > "$tmp/all-bytes"
# most control bytes present: few free bytes, the codes must fit what is left
python3 -c "import sys; sys.stdout.buffer.write(bytes(range(1, 31)) + b'The Quick Brown Fox jumps over the lazy dog. ' * 8000)" > "$tmp/few-free"

fail=0
for f in empty one-byte random-64k zeros-1m no-newlines high-bytes wikiish caps-ents long-words all-bytes few-free; do
  in="$tmp/$f"
  if "$bin" c "$in" "$in.cmp" 2>/dev/null && "$bin" d "$in.cmp" "$in.out" 2>/dev/null && cmp -s "$in" "$in.out"; then
    printf 'ok    %-12s %9d -> %9d bytes\n' "$f" "$(stat -c %s "$in")" "$(stat -c %s "$in.cmp")"
  else
    printf 'FAIL  %-12s\n' "$f"
    fail=1
  fi
done
[ "$fail" -eq 0 ] && echo "all edge cases round-trip" || { echo "EDGE CASE FAILURE" >&2; exit 1; }

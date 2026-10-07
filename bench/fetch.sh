#!/usr/bin/env bash
# Downloads enwik9 into data/ and cuts the smaller development slices.
#   enwik9 = 10^9 bytes (the contest file)
#   enwik8 = first 10^8 bytes (the old contest file, quick full-scale test)
#   enwik7 = first 10^7 bytes (fast iteration)
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p data
cd data

if [ ! -f enwik9 ]; then
  [ -f enwik9.zip ] || curl -fL --retry 3 -o enwik9.zip http://mattmahoney.net/dc/enwik9.zip
  python3 -c "import zipfile; zipfile.ZipFile('enwik9.zip').extract('enwik9')"
fi

size=$(stat -c %s enwik9)
[ "$size" -eq 1000000000 ] || { echo "enwik9 has $size bytes, expected 1000000000" >&2; exit 1; }

[ -f enwik8 ] || head -c 100000000 enwik9 > enwik8
[ -f enwik7 ] || head -c 10000000 enwik9 > enwik7
sha256sum enwik9 enwik8 enwik7

#!/usr/bin/env bash
# Builds bin/cm1. One binary does both compression (c) and decompression (d),
# so per the contest FAQ the score is size(binary) + size(archive).
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
gcc -O2 -std=c11 -Wall -Wextra -s ${CFLAGS:-} -o bin/cm1 cm1.c

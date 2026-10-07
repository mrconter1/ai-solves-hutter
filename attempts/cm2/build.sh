#!/usr/bin/env bash
# Builds bin/cm2. One binary does both compression (c) and decompression (d),
# so per the contest FAQ the score is size(binary) + size(archive).
#
# The binary is static, because the contest wants a self-contained program and
# bench/run.sh runs it chrooted with no libraries. musl keeps it small (tens of
# KB); glibc static works too but adds ~730 KB of runtime to the score.
#   sudo apt install musl-tools
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
cc=gcc
command -v musl-gcc >/dev/null && cc=musl-gcc
$cc -O2 -std=c11 -Wall -Wextra -s -static ${CFLAGS:-} -o bin/cm2 cm2.c

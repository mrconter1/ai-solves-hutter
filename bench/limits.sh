# Contest limits, as enforced by bench/run.sh. Sourced, not executed.

# Time: the prize page allows about 50 hours per direction on the test
# machine, timed on "a 2.7 GHz i7" class core.
TIME_LIMIT_H=${TIME_LIMIT_H:-50}

# How much faster the machine running the benchmark is than the contest's test
# machine, single core. The limit applied is TIME_LIMIT_H / SPEED_FACTOR.
#
# Measured 2026-10-07, cm2 on enwik7: 14 s per direction on a Google Cloud
# n2d (AMD EPYC 7B13), 29 s on the dev laptop (Ryzen 5 PRO 7540U under WSL).
# The contest's reference is "a 2.7 GHz i7" class core; we assume it is about
# as fast as the n2d, so the default is 1.0. That assumption is unmeasured.
# A slower machine (like the laptop) only makes the limit more generous than
# the contest's, so release timings should come from the cloud.
SPEED_FACTOR=${SPEED_FACTOR:-1.0}

# Memory: under 10 GB working RAM. Applied as an address-space cap (ulimit -v),
# which is stricter than resident memory.
MEM_LIMIT_GB=${MEM_LIMIT_GB:-10}

# Disk: under 100 GB. Measured as the peak size of the sandbox directory,
# which holds the binary, its input, its output and any temp files.
DISK_LIMIT_GB=${DISK_LIMIT_GB:-100}

# How often to sample disk use, in seconds.
DISK_SAMPLE_S=${DISK_SAMPLE_S:-5}

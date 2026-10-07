# Contest limits, as enforced by bench/run.sh. Sourced, not executed.

# Time: the prize page allows about 50 hours per direction on the test
# machine, timed on "a 2.7 GHz i7" class core.
TIME_LIMIT_H=${TIME_LIMIT_H:-50}

# How much faster this machine is than the contest's test machine, single
# core. The limit applied here is TIME_LIMIT_H / SPEED_FACTOR.
#
# ESTIMATE, not measured: the dev laptop is a Ryzen 5 PRO 7540U (Zen 4, up to
# 4.9 GHz). Against a 2.7 GHz-class Skylake/Kaby Lake i7 that is roughly
# 1.3x clock x 1.4x IPC, so about 2x. A run that fits in 25 h here should
# fit in 50 h there. The FAQ's own rule of thumb (500,000 / GeekBench 5 score
# hours) can replace this once a score for both machines is at hand.
SPEED_FACTOR=${SPEED_FACTOR:-2.0}

# Memory: under 10 GB working RAM. Applied as an address-space cap (ulimit -v),
# which is stricter than resident memory.
MEM_LIMIT_GB=${MEM_LIMIT_GB:-10}

# Disk: under 100 GB. Measured as the peak size of the sandbox directory,
# which holds the binary, its input, its output and any temp files.
DISK_LIMIT_GB=${DISK_LIMIT_GB:-100}

# How often to sample disk use, in seconds.
DISK_SAMPLE_S=${DISK_SAMPLE_S:-5}

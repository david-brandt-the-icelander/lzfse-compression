#!/usr/bin/env bash
# The CI fuzz job, kept short: each of the four libFuzzer targets (decode, round_trip, lzvn_decode, decmpfs) for
# FUZZ_SECONDS (30 by default, so about two minutes of fuzzing after a one-minute build). On a crash the log shows
# a hex dump of the input, because a failed step's output is not uploaded.
set -euo pipefail
export HOME=/tmp
srcs=(/zazzle/input/repos/*/lzfse-compression); src=${srcs[0]}
cp -R "$src" /tmp/lzfse
cd /tmp/lzfse
dump() {
  for f in crash-* leak-* timeout-* oom-* slow-unit-*; do
    [ -f "$f" ] || continue
    echo "fuzz: $f ($(wc -c <"$f") bytes), first 4 KiB:"; head -c 4096 "$f" | xxd
  done
}
trap dump EXIT
make -j2 fuzz FUZZ_SECONDS="${FUZZ_SECONDS:-30}"
echo "fuzz: clean after ${FUZZ_SECONDS:-30} s per target"

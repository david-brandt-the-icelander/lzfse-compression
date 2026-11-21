#!/bin/bash
# Branch coverage of the library (src/*.c) from the unit, allocation and out-of-memory tests, with clang's source-based
# coverage. Prints a per-file summary and writes the uncovered branches (file, line, which side never ran) to
# build/coverage/missed.txt. Needs clang, llvm-profdata and llvm-cov of one LLVM version: set LLVM_BIN to their
# directory (for example /opt/homebrew/opt/llvm/bin); by default they are found on PATH.
set -e
cd "$(dirname "$0")/.."
BIN=${LLVM_BIN:+$LLVM_BIN/}
OUT=build/coverage
mkdir -p "$OUT"
rm -f "$OUT"/*.profraw
FLAGS="-std=c11 -D_DEFAULT_SOURCE -O0 -g -fprofile-instr-generate -fcoverage-mapping -Iinclude -Isrc -Itools -Itests -pthread"
LIBS="-pthread"
[ "$(uname -s)" = Darwin ] && APPLE_LIBS=-lcompression
${BIN}clang $FLAGS -o "$OUT/unit-tests" tests/unit_tests.c src/*.c $LIBS $APPLE_LIBS
${BIN}clang $FLAGS -DLZFSE_ZAZZCAT_COUNT_ALLOCATIONS -o "$OUT/alloc-tests" tests/alloc_tests.c src/*.c $LIBS
${BIN}clang $FLAGS -DLZFSE_ZAZZCAT_COUNT_ALLOCATIONS -o "$OUT/oom-tests" tests/oom_tests.c src/*.c $LIBS
LLVM_PROFILE_FILE="$OUT/unit.profraw" "$OUT/unit-tests" > "$OUT/unit-tests.log" 2>&1
LLVM_PROFILE_FILE="$OUT/alloc.profraw" "$OUT/alloc-tests" > "$OUT/alloc-tests.log" 2>&1
LLVM_PROFILE_FILE="$OUT/oom-pool.profraw" "$OUT/oom-tests" pool > "$OUT/oom-pool.log" 2>&1
LLVM_PROFILE_FILE="$OUT/oom.profraw" "$OUT/oom-tests" > "$OUT/oom-tests.log" 2>&1
${BIN}llvm-profdata merge -o "$OUT/all.profdata" "$OUT"/*.profraw
OBJECTS="-object $OUT/alloc-tests -object $OUT/oom-tests"
# The counted-allocation builds differ from the unit-test build, so llvm-cov warns about mismatched functions.
${BIN}llvm-cov report "$OUT/unit-tests" $OBJECTS -instr-profile="$OUT/all.profdata" src/*.c 2>/dev/null |
    awk 'NR == 1 {print "file branches missed cover"} NR > 2 {print $1, $11, $12, $13}' | column -t
${BIN}llvm-cov show "$OUT/unit-tests" $OBJECTS -instr-profile="$OUT/all.profdata" src/*.c -show-branches=count 2>/dev/null |
    awk '/^\/.*\.c:$/ {f=$0; sub(/.*\//,"",f)} /^ *\| *Branch \(.*(True|False): 0[,\]]/ {print f, $0}' |
    sed 's/ *| *Branch//' | sort -u > "$OUT/missed.txt"
echo "uncovered branches: $(wc -l < "$OUT/missed.txt") (listed in $OUT/missed.txt)"

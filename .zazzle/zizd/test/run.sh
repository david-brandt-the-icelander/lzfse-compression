#!/usr/bin/env bash
# The CI test job's Linux leg: unit and round-trip tests, allocation and out-of-memory tests, and the tests under
# AddressSanitizer and UndefinedBehaviorSanitizer (with gcc, as CI does on Ubuntu). The macOS leg waits for macOS VM steps.
set -euo pipefail
export HOME=/tmp
srcs=(/zazzle/input/repos/*/lzfse-compression); src=${srcs[0]}
cp -R "$src" /tmp/lzfse
cd /tmp/lzfse
make -j2 test
make -j2 test-alloc
make -j2 test-sanitize CC=gcc
echo "test: passed"

#!/usr/bin/env bash
# The CI lint job: formatting, warnings-as-errors builds with gcc, clang and without threads, and shellcheck.
# Leaves the gcc build (library, header, CLI, benchmark) as a tarball for the reference step and for people.
set -euo pipefail
export HOME=/tmp
srcs=(/zazzle/input/repos/*/lzfse-compression); src=${srcs[0]}
cp -R "$src" /tmp/lzfse
cd /tmp/lzfse
make format-check
make -j2 CC=gcc BUILD=build/gcc all
make -j2 CC=clang BUILD=build/clang all
make -j2 CC=clang BUILD=build/no-threads DEFINES=-DLZFSE_ZAZZCAT_NO_THREADS all
shellcheck scripts/*.sh

name=lzfse-zazzcat-linux-$(uname -m)
mkdir -p "/tmp/$name/lib" "/tmp/$name/include" "/tmp/$name/bin"
cp build/gcc/liblzfse_zazzcat.a "/tmp/$name/lib/"
cp include/lzfse_zazzcat.h "/tmp/$name/include/"
cp build/gcc/lzfse-zazzcat build/gcc/lzfse-bench "/tmp/$name/bin/"
{ echo "commit ${ZAZZLE_COMMIT:-$(git rev-parse HEAD)}"; gcc --version | head -1; clang --version | head -1
  clang-format --version; shellcheck --version | grep '^version'; } > "/tmp/$name/BUILD"
tar -C /tmp -czf "/zazzle/output/$name.tar.gz" "$name"
cat "/tmp/$name/BUILD"
echo "lint: passed; left $name.tar.gz"

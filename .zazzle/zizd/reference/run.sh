#!/usr/bin/env bash
# The CI reference job: streams the lint step's CLI encodes must decode with Apple's reference implementation
# (built into this image), and streams the reference encodes must decode with ours.
set -euo pipefail
export HOME=/tmp
srcs=(/zazzle/input/repos/*/lzfse-compression); src=${srcs[0]}
tarballs=(/zazzle/input/artifacts/lint/lzfse-zazzcat-linux-*.tar.gz); tarball=${tarballs[0]}
[ -f "$tarball" ] || { echo "reference: no lint artifact (lzfse-zazzcat-linux-*.tar.gz) in /zazzle/input/artifacts/lint" >&2; exit 1; }
mkdir -p /tmp/dist
tar -C /tmp/dist -xzf "$tarball"
ours=(/tmp/dist/*/bin/lzfse-zazzcat)
bash "$src/scripts/compat-reference.sh" /opt/lzfse/build/bin/lzfse "${ours[0]}"
echo "reference: passed (reference lzfse at $(cat /opt/lzfse/COMMIT))"

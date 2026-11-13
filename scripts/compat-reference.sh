#!/usr/bin/env bash
# Cross-check against Apple's BSD reference implementation (github.com/lzfse/lzfse) on Linux:
# streams we encode must decode with the reference, and streams the reference encodes must decode with ours.
# Usage: scripts/compat-reference.sh <path to built reference `lzfse` binary> <path to our `lzfse-zazzcat` binary>
set -euo pipefail
ref=$1
ours=$2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Build a small corpus: text, binary, noise, zeros and every size threshold.
python3 - "$work" <<'PY'
import os, random, sys
d = sys.argv[1]
r = random.Random(7)
words = ["the ", "quick ", "brown ", "fox ", "jumps ", "over ", "a ", "lazy ", "dog.\n"]
text = "".join(r.choice(words) for _ in range(200000)).encode()
files = {
    "zeros": bytes(1 << 20),
    "noise": os.urandom(300000),
    "base64": bytes(r.choice(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") for _ in range(300000)),
    "binary": open(sys.executable, "rb").read()[:2000000],
}
for n in [1, 7, 8, 100, 4095, 4096, 4097, 65536, 1000000]:
    files[f"text{n}"] = text[:n]
for name, data in files.items():
    open(os.path.join(d, name), "wb").write(data)
PY

fail=0
for f in "$work"/*; do
  [ -f "$f" ] || continue
  case "$f" in *.ours|*.ref|*.out*) continue ;; esac
  "$ours" encode "$f" "$f.ours"
  "$ref" -decode -i "$f.ours" -o "$f.out1"
  "$ref" -encode -i "$f" -o "$f.ref"
  "$ours" decode "$f.ref" "$f.out2"
  for out in "$f.out1" "$f.out2"; do
    if ! cmp -s "$f" "$out"; then
      echo "MISMATCH: $(basename "$f") via $(basename "$out")"
      fail=1
    fi
  done
  printf '%-10s %9d bytes  ours %9d  reference %9d\n' "$(basename "$f")" "$(stat -c %s "$f")" "$(stat -c %s "$f.ours")" "$(stat -c %s "$f.ref")"
done
exit $fail

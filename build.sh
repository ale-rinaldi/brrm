#!/usr/bin/env bash
# Compila brrm e brrm-partenza con Gambas 3.
# Esce non-zero se uno dei due progetti non compila.
# Pensato per essere usato in CI o in locale.

set -euo pipefail

cd "$(dirname "$0")"

projects=(brrm brrm-partenza)
fail=0

for p in "${projects[@]}"; do
  echo "=== $p ==="
  pushd "$p" >/dev/null
  if gbc3 -w -a; then
    gba3
    echo "OK -> $p/$p.gambas"
  else
    echo "FAIL: $p"
    fail=$((fail+1))
  fi
  popd >/dev/null
  echo
done

if [ "$fail" -ne 0 ]; then
  echo "$fail progetto/i con errori di compilazione."
  exit 1
fi
echo "Tutti i progetti compilati correttamente."

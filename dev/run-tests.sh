#!/usr/bin/env bash
# Esegue la suite di unit test (brrm-utils-test/) nativamente.
# Richiede gbc3 e gbx3 nel PATH, e i componenti Gambas: gb.test + gb.pcre.
# Esce con codice non-zero se qualche assertion fallisce o se il compile
# fallisce.
#
# Pensato per CI (dove il container ha già gambas3-runtime installato)
# e per dev su Linux con Gambas locale.
# Per dev su Mac: usa dev/run-tests-docker.sh (wrapper sopra a questo).
set -euo pipefail
cd "$(dirname "$0")/.."

if ! command -v gbc3 >/dev/null 2>&1 || ! command -v gbx3 >/dev/null 2>&1; then
  echo "ERROR: gbc3 o gbx3 non trovati nel PATH." >&2
  echo "Installa gambas3-runtime + gambas3-gb-pcre, oppure usa dev/run-tests-docker.sh." >&2
  exit 2
fi

cd brrm-utils-test
rm -rf .gambas
gbc3 -ag . >/dev/null

# gbx3 NON propaga l'exit code di TAP failures. Catturiamo l'output,
# stampiamolo, e cerchiamo righe "not ok" per derivare il successo.
out=$(mktemp)
trap 'rm -f "$out"' EXIT
gbx3 . | tee "$out"
fails=$(grep -c '^not ok ' "$out" || true)
if [ "$fails" -gt 0 ]; then
  echo
  echo "FAIL: $fails test falliti." >&2
  exit 1
fi
# Verifica anche che il plan numerico sia coerente col numero di ok
plan=$(awk '/^1\.\.[0-9]+$/ { print substr($0, 4); exit }' "$out")
oks=$(grep -c '^ok ' "$out" || true)
if [ -n "$plan" ] && [ "$oks" -ne "$plan" ]; then
  echo "FAIL: Test.Plan($plan) ma solo $oks ok trovati (test mancanti?)" >&2
  exit 1
fi
echo "PASS: $oks/$plan test."

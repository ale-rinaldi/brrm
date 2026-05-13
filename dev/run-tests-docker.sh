#!/usr/bin/env bash
# Wrapper docker per dev/run-tests.sh: utile su Mac (o ovunque manchi
# un Gambas locale). Usa brrm-dev-test:latest (definita in dev/test/).
# Builda l'image se manca.
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE=brrm-dev-test:latest
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "[run-tests-docker] image $IMAGE assente, la costruisco..."
  docker build -t "$IMAGE" dev/test
fi

exec docker run --rm \
  -v "$PWD:/workspace" \
  -w /workspace \
  "$IMAGE" \
  bash dev/run-tests.sh

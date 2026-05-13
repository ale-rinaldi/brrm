#!/usr/bin/env bash
# Esegue brrm dentro un container che usa il NOSTRO Gambas master
# (con gb.openssl-con-RSA) buildato from-source. Serve per testare il
# branch feature/gb-openssl-native-jwt finche' gb.openssl 3.21.99+ non
# arriva in Debian.
#
# Uso:
#   ./dev/test-rsa/run.sh                  # default: build (se serve) + run brrm + VNC
#   ./dev/test-rsa/run.sh --build          # forza rebuild dell'image
#   ./dev/test-rsa/run.sh --shell          # apre una shell interattiva
#
# VNC: localhost:5910 (password: brrm-dev)
set -euo pipefail
cd "$(dirname "$0")/../.."

IMAGE=brrm-dev-rsa:latest
NAME=brrm-dev-rsa-test
VNC_PORT=5910

FORCE_BUILD=false
MODE=run
for arg in "$@"; do
  case "$arg" in
    --build) FORCE_BUILD=true ;;
    --shell) MODE=shell ;;
  esac
done

if $FORCE_BUILD || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "[run.sh] build dell'image $IMAGE (lungo: ~15-20 min)..."
  docker build -t "$IMAGE" dev/test-rsa
fi

mkdir -p "$HOME/.config/brrm"
STATE_DIR="$PWD/dev/test-rsa/state"
mkdir -p "$STATE_DIR/.config/brrm"

docker rm -f "$NAME" >/dev/null 2>&1 || true

if [ "$MODE" = "shell" ]; then
  echo "[run.sh] shell interattiva..."
  exec docker run --rm -it \
    -v "$PWD:/workspace" \
    -v "$STATE_DIR:/home/boxrally" \
    -v "$HOME/.config/brrm:/home/boxrally/.config/brrm" \
    --entrypoint "" \
    "$IMAGE" bash
fi

echo "[run.sh] compilo brrm nel container..."
docker run --rm \
  -v "$PWD:/workspace" \
  -w /workspace/brrm \
  --entrypoint "" \
  "$IMAGE" \
  bash -c "cp .icon.png .app.png 2>/dev/null; gbc3 -w -e -a -g -t -fpublic-control -fpublic-module -x -j1 && gba3"

echo "[run.sh] avvio brrm (VNC su localhost:$VNC_PORT)..."
docker run -d \
  -p "127.0.0.1:$VNC_PORT:5900" \
  -v "$PWD:/workspace" \
  -v "$STATE_DIR:/home/boxrally" \
  -v "$HOME/.config/brrm:/home/boxrally/.config/brrm" \
  --name "$NAME" \
  "$IMAGE" \
  gbr3 /workspace/brrm/brrm.gambas >/dev/null

for i in $(seq 1 30); do
  nc -z 127.0.0.1 "$VNC_PORT" 2>/dev/null && break
  sleep 0.2
done

echo "[run.sh] apro Screen Sharing su vnc://localhost:$VNC_PORT (password: brrm-dev)"
open "vnc://localhost:$VNC_PORT" 2>/dev/null || true
echo "[run.sh] Ctrl+C per fermare. Stato live:"
trap 'docker stop "$NAME" >/dev/null 2>&1 || true' EXIT
docker logs -f "$NAME" 2>&1 || true

#!/usr/bin/env bash
# Esegue brrm o brrm-partenza dentro un container Linux. L'X server gira
# *dentro* il container (Xvfb), e Screen Sharing.app del Mac si connette
# via VNC per visualizzarlo. Niente dipendenza da XQuartz — risolve i bug
# di sync surface Cocoa che lasciano nere le modali Qt6 (Message.Question).
#
# Prerequisiti: solo Docker Desktop. Screen Sharing.app e' nativa su macOS.
#
# Uso:
#   ./dev/osx/run.sh                # default: brrm (postazione arrivo)
#   ./dev/osx/run.sh brrm-partenza  # postazione partenza
#   ./dev/osx/run.sh --build        # forza rebuild dell'image
#
# Porte VNC: brrm -> localhost:5900, brrm-partenza -> localhost:5901
# (mappate solo su 127.0.0.1, mai esposte sulla rete).

set -euo pipefail

cd "$(dirname "$0")/../.."

IMAGE=brrm-dev:latest
PROJECT="${1:-brrm}"
FORCE_BUILD=false

for arg in "$@"; do
  case "$arg" in
    --build) FORCE_BUILD=true ;;
  esac
done

case "$PROJECT" in
  brrm)          VNC_PORT=5900 ;;
  brrm-partenza) VNC_PORT=5901 ;;
  --build) PROJECT="brrm";       VNC_PORT=5900 ;;
  *) echo "Progetto sconosciuto: $PROJECT. Usa 'brrm' o 'brrm-partenza'." >&2; exit 2 ;;
esac

# --- build dell'image ------------------------------------------------------

if $FORCE_BUILD || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "Build dell'image $IMAGE..."
  docker build -t "$IMAGE" dev/osx
fi

# --- compile sources (in container) ----------------------------------------
# Replica esatta dei flag di build.sh (CI), cosi' il comportamento e'
# identico ai .deb di produzione.

echo "Compilo $PROJECT nel container..."
docker run --rm \
  -v "$PWD:/workspace" \
  -w "/workspace/$PROJECT" \
  --entrypoint "" \
  "$IMAGE" \
  bash -c "cp .icon.png .app.png 2>/dev/null; gbc3 -w -e -a -g -t -fpublic-control -fpublic-module -x -j1 && gba3"

# --- prepara mount ---------------------------------------------------------

mkdir -p "$HOME/.config/brrm"
STATE_DIR="$PWD/dev/osx/state/$PROJECT"
mkdir -p "$STATE_DIR/.config/brrm"

# --- ferma eventuale istanza precedente -----------------------------------

docker rm -f "brrm-dev-$PROJECT" >/dev/null 2>&1 || true

# --- run -------------------------------------------------------------------

echo "Avvio $PROJECT (VNC su localhost:$VNC_PORT)..."

# Container detached: cosi' apro Screen Sharing prima che l'app blocchi
# il foreground.
docker run -d \
  -p "127.0.0.1:$VNC_PORT:5900" \
  -v "$PWD:/workspace" \
  -v "$STATE_DIR:/home/boxrally" \
  -v "$HOME/.config/brrm:/home/boxrally/.config/brrm" \
  --name "brrm-dev-$PROJECT" \
  "$IMAGE" \
  gbr3 "/workspace/$PROJECT/$PROJECT.gambas" >/dev/null

# Aspetta che x11vnc sia up
for i in $(seq 1 30); do
  if nc -z 127.0.0.1 "$VNC_PORT" 2>/dev/null; then break; fi
  sleep 0.2
done

echo "Apro Screen Sharing su vnc://localhost:$VNC_PORT (password: brrm-dev — Ctrl+C per fermare)"
open "vnc://localhost:$VNC_PORT"

# Segui il container in foreground: Ctrl+C lo ferma
trap 'docker stop "brrm-dev-$PROJECT" >/dev/null 2>&1 || true' EXIT
docker logs -f "brrm-dev-$PROJECT" 2>&1 || true

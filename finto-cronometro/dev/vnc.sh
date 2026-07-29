#!/usr/bin/env bash
# Prova la FINESTRA nativa dentro un container Linux, guardandola via VNC.
#
# Stesso schema di dev/osx/run.sh del legacy — Xvfb dentro il container, x11vnc
# per Screen Sharing.app — e per la stessa ragione: serve un X server vero. Gio ha
# bisogno di una sessione grafica, e su un Mac lanciato da un terminale senza
# accesso al window server va in panico dentro la creazione della finestra.
#
# Uso:
#   ./dev/vnc.sh              # avvia e apre Screen Sharing
#   ./dev/vnc.sh --build      # ricostruisce l'image
#   ./dev/vnc.sh --scatto     # cattura una schermata e esce (per i controlli)
#
# VNC su 127.0.0.1:5902, password brrm-dev. La porta è mappata solo su loopback.

set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE=finto-cronometro-gui:latest
NOME=finto-cronometro-gui
PORTA=5902

for a in "$@"; do [ "$a" = "--build" ] && FORZA=1; done
if [ "${FORZA:-}" = 1 ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "Build dell'image $IMAGE..."
  docker build -t "$IMAGE" dev
fi

docker rm -f "$NOME" >/dev/null 2>&1 || true
echo "Compilo con -tags gui e avvio nel container..."
docker run -d --name "$NOME" -p "127.0.0.1:$PORTA:5900" \
  -v "$PWD:/src" -w /src "$IMAGE" \
  bash -c 'go build -tags gui -o /tmp/fc . && /tmp/fc -finestra -addr 0.0.0.0:8099' >/dev/null

# La prima volta la compilazione scarica Gio: si aspetta la finestra, non il
# container. Senza questa attesa lo scatto arriva su uno schermo vuoto.
for i in $(seq 1 120); do
  if docker exec "$NOME" bash -c 'DISPLAY=:99 xdotool search --name "finto cronometro"' >/dev/null 2>&1; then
    break
  fi
  sleep 1
done

if [ "${1:-}" = "--scatto" ]; then
  docker exec "$NOME" bash -c 'DISPLAY=:99 scrot -o /tmp/win.png'
  docker cp "$NOME:/tmp/win.png" ./finestra.png
  docker rm -f "$NOME" >/dev/null
  echo "schermata in ./finestra.png"
  exit 0
fi

echo "Apro Screen Sharing su vnc://localhost:$PORTA (password: brrm-dev — Ctrl+C per fermare)"
open "vnc://localhost:$PORTA" 2>/dev/null || echo "  (su Linux: usa un client VNC su localhost:$PORTA)"
trap 'docker rm -f "$NOME" >/dev/null 2>&1 || true' EXIT
docker logs -f "$NOME" 2>&1 || true

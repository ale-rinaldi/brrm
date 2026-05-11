#!/usr/bin/env bash
# Esegue brrm o brrm-partenza dentro un container Linux, con la finestra
# Qt6 forwarded a XQuartz sull'host macOS. Pensato per sviluppo e test del
# sync UI senza dipendere dai PC remoti.
#
# Prerequisiti (one-time):
#   1. Installare XQuartz da https://www.xquartz.org/
#   2. In XQuartz Preferences → Security: spuntare "Allow connections
#      from network clients" (oppure: il primo lancio di questo script
#      lo configura automaticamente).
#   3. Logout/login dopo l'install di XQuartz cosi' il PATH include /opt/X11/bin.
#
# Uso:
#   ./dev/osx/run.sh                # default: brrm (postazione arrivo)
#   ./dev/osx/run.sh brrm-partenza  # postazione partenza
#   ./dev/osx/run.sh --build        # forza rebuild dell'image
#
# Il config sync (~/.config/brrm/align.conf) sul Mac viene mountato nel
# container in sola lettura cosi' le credenziali sono condivise con i PC
# remoti durante i test.

set -euo pipefail

cd "$(dirname "$0")/../.."   # vai alla root del repo

IMAGE=brrm-dev:latest
PROJECT="${1:-brrm}"
FORCE_BUILD=false

for arg in "$@"; do
  case "$arg" in
    --build) FORCE_BUILD=true ;;
  esac
done

case "$PROJECT" in
  brrm|brrm-partenza) ;;
  --build) PROJECT="brrm" ;;
  *) echo "Progetto sconosciuto: $PROJECT. Usa 'brrm' o 'brrm-partenza'." >&2; exit 2 ;;
esac

# --- XQuartz check ---------------------------------------------------------

XHOST=/opt/X11/bin/xhost
if [[ ! -x "$XHOST" ]]; then
  echo "XQuartz non trovato in /opt/X11/. Installa da https://www.xquartz.org/" >&2
  exit 3
fi

if ! pgrep -fq /opt/X11/bin/Xquartz; then
  echo "Avvio XQuartz..."
  open -a XQuartz
  # Attendi che il server sia in listen su :6000
  for i in {1..15}; do
    if lsof -nP -iTCP:6000 -sTCP:LISTEN >/dev/null 2>&1; then break; fi
    sleep 1
  done
fi

# Assicurati che XQuartz accetti connessioni TCP (richiede restart se cambia)
if [[ "$(defaults read org.xquartz.X11 nolisten_tcp 2>/dev/null || echo 1)" != "0" ]]; then
  echo "Configuro XQuartz per accettare connessioni TCP (riavvio una tantum)..."
  defaults write org.xquartz.X11 nolisten_tcp -bool false
  pkill -f /opt/X11/bin/Xquartz || true
  sleep 2
  open -a XQuartz
  for i in {1..15}; do
    if lsof -nP -iTCP:6000 -sTCP:LISTEN >/dev/null 2>&1; then break; fi
    sleep 1
  done
fi

# Autorizza il container a connettersi al display
DISPLAY=:0 "$XHOST" +localhost >/dev/null

# --- build dell'image ------------------------------------------------------

if $FORCE_BUILD || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "Build dell'image $IMAGE..."
  docker build -t "$IMAGE" dev/osx
fi

# --- compile sources (in container) ----------------------------------------
# Compila ogni volta cosi' modifiche al codice sono immediatamente effettive.
# Replica esatta dei flag di build.sh (CI).

echo "Compilo $PROJECT nel container..."
docker run --rm \
  -v "$PWD:/workspace" \
  -w "/workspace/$PROJECT" \
  "$IMAGE" \
  bash -c "cp .icon.png .app.png 2>/dev/null; gbc3 -w -e -a -g -t -fpublic-control -fpublic-module -x -j1 && gba3"

# --- run -------------------------------------------------------------------

# Config sync condiviso col Mac: assicura che la cartella esista (cosi' la
# form "Impostazioni..." dentro l'app puo' scriverci) e mountala read-write
# come sub-mount sopra la home persistente.
mkdir -p "$HOME/.config/brrm"

# Stato di sessione (~/.brrm-session, ~/.brrm-align-lastseq, ecc.):
# persiste in dev/osx/state/<project> cosi' restart del container conserva
# l'ordine di partenze, gli orari, e last_seq del sync.
STATE_DIR="$PWD/dev/osx/state/$PROJECT"
mkdir -p "$STATE_DIR/.config/brrm"

echo "Avvio $PROJECT — finestra su XQuartz..."
TTYFLAG=()
[[ -t 0 ]] && TTYFLAG=( -t )
docker run --rm -i "${TTYFLAG[@]}" \
  -e DISPLAY=host.docker.internal:0 \
  -v "$PWD:/workspace" \
  -v "$STATE_DIR:/home/boxrally" \
  -v "$HOME/.config/brrm:/home/boxrally/.config/brrm" \
  --name "brrm-dev-$PROJECT" \
  "$IMAGE" \
  gbr3 "/workspace/$PROJECT/$PROJECT.gambas"

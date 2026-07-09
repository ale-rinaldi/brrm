#!/usr/bin/env bash
# Arduino virtuale: implementa il protocollo del "clock interno" dietro una
# pty creata da socat, per testare il layer seriale di brrm/brrm-partenza
# senza hardware reale.
#
# Uso (dentro il container dev/osx, dove socat è installato):
#   socat PTY,link=/dev/ttyACM0,raw,echo=0,perm=0666 \
#         EXEC:'/workspace/dev/fake-arduino.sh',pty
#
# stdin  = byte dei comandi dal PC (?, @, #)
# stdout = risposte verso il PC (K.., D.., Y..) e passaggi asincroni (P..)
#
# Canale di controllo (FIFO $FAKE_CTL, default /tmp/fake-ctl): una riga per
# comando, per pilotare lo scenario da un altro processo:
#   passage        -> emette un passaggio P<unix>.<ms><src> (src G o R)
#   src G|R|N       -> cambia la sorgente di tempo riportata nei pong K
#   hang            -> smette di rispondere (simula firmware appeso)
#   resume          -> torna a rispondere
#   legacybyte      -> emette un singolo byte grezzo (per la modalità legacy)
#
# I file di stato ($FAKE_SRC, $FAKE_HANG) sono condivisi tra il loop principale
# e il gestore del canale di controllo (bash non condivide variabili tra
# subshell, quindi lo stato vive su file).

set -u
CTL=${FAKE_CTL:-/tmp/fake-ctl}
SRCF=${FAKE_SRC:-/tmp/fake-src}
HANGF=${FAKE_HANG:-/tmp/fake-hang}

echo "${FAKE_INIT_SRC:-G}" > "$SRCF"
rm -f "$HANGF"
[ -p "$CTL" ] || { rm -f "$CTL"; mkfifo "$CTL"; }

emit_pong()    { printf 'K%s%s.%s\n' "$(cat "$SRCF")" "$(date -u +%s)" "$(date -u +%3N)"; }
emit_diag()    { printf 'D%s,%s,0,187,A,9,0.93,142,0,12,3,57\n' "$(date -u +%s)" "$(cat "$SRCF")"; }
emit_id()      { printf 'Y1\n'; }
emit_passage() {
  local s; s=$(cat "$SRCF")
  [ "$s" = "N" ] && s=G   # un passaggio deve avere src G o R
  printf 'P%s.%s%s\n' "$(date -u +%s)" "$(date -u +%3N)" "$s"
}

# Gestore del canale di controllo: scrive sul pty (fd 1, ereditato).
(
  while :; do
    if read -r cmd arg < "$CTL"; then
      case "$cmd" in
        passage)    [ -f "$HANGF" ] || emit_passage ;;
        src)        echo "$arg" > "$SRCF" ;;
        hang)       touch "$HANGF" ;;
        resume)     rm -f "$HANGF" ;;
        legacybyte) printf 'X' ;;
      esac
    fi
  done
) &
CTLPID=$!
trap 'kill "$CTLPID" 2>/dev/null; rm -f "$CTL"' EXIT

# Loop principale: risponde ai comandi request/response dal PC.
while IFS= read -r -n1 c; do
  [ -f "$HANGF" ] && continue
  case "$c" in
    '?') emit_pong ;;
    '@') emit_diag ;;
    '#') emit_id ;;
  esac
done

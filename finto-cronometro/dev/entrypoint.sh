#!/usr/bin/env bash
# Xvfb + fluxbox + x11vnc, poi il comando passato. Copiato dallo schema del
# legacy (dev/osx/entrypoint.sh) perché lì era già stato tarato: senza un window
# manager le finestre si aprono fuori dallo schermo o senza cornice.
set -e
export DISPLAY=:99

Xvfb :99 -screen 0 900x760x24 -ac +extension RANDR +extension RENDER >/tmp/xvfb.log 2>&1 &
for i in $(seq 1 50); do
  xdpyinfo -display :99 >/dev/null 2>&1 && break
  sleep 0.1
done

fluxbox >/tmp/fluxbox.log 2>&1 &

# Password fissa: Screen Sharing del Mac chiede sempre l'autenticazione, anche con
# -nopw. Il mapping di Docker espone la porta solo su 127.0.0.1.
x11vnc -storepasswd brrm-dev /tmp/vnc.pass >/dev/null 2>&1
x11vnc -display :99 -forever -shared -rfbauth /tmp/vnc.pass -quiet \
       -rfbport 5900 -listen 0.0.0.0 >/tmp/x11vnc.log 2>&1 &

exec "$@"

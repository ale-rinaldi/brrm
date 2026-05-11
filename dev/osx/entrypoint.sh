#!/usr/bin/env bash
# Entrypoint del container brrm-dev: avvia Xvfb (X server virtuale, niente
# dipendenza da XQuartz sull'host), fluxbox come WM minimo, x11vnc per
# esporre il display a Screen Sharing.app del Mac, e infine l'app passata
# come argomento. L'app diventa il processo principale (PID 1 via exec):
# quando esce, il container termina pulito.
set -e

export DISPLAY=:99

Xvfb :99 -screen 0 1280x800x24 -ac +extension RANDR +extension RENDER >/tmp/xvfb.log 2>&1 &

# Aspetta che Xvfb sia pronto (di solito <100ms)
for i in $(seq 1 50); do
  if xdpyinfo -display :99 >/dev/null 2>&1; then break; fi
  sleep 0.1
done

# WM leggero: senza, le modali Qt6 si aprono fuori dallo schermo o senza chrome
fluxbox >/tmp/fluxbox.log 2>&1 &

# VNC: bind 0.0.0.0 perche' il mapping di Docker fa girare le connessioni
# da host.docker.internal; ci pensa Docker a esporre la porta solo su 127.0.0.1
# lato host (-p 127.0.0.1:PORT:5900 nel run.sh). Password fissa "brrm-dev"
# perche' Screen Sharing.app del Mac chiede sempre l'auth (anche con -nopw):
# meglio una password nota che far perdere tempo all'utente. Il mapping
# loopback-only rende comunque il VNC inaccessibile dalla rete.
x11vnc -storepasswd brrm-dev /tmp/vnc.pass >/dev/null 2>&1
x11vnc -display :99 -forever -shared -rfbauth /tmp/vnc.pass -quiet \
       -rfbport 5900 -listen 0.0.0.0 >/tmp/x11vnc.log 2>&1 &

# L'app: PID principale del container
exec "$@"

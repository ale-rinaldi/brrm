#!/usr/bin/env bash
# Genera pacchetti .deb per brrm e brrm-partenza.
# Richiede: gambas3 (gbc3, gba3), dpkg-deb, fakeroot.
# Output: dist/<progetto>_<versione>_all.deb

set -euo pipefail

cd "$(dirname "$0")"

# Compila prima
./build.sh

mkdir -p dist

# Metadata
MAINTAINER="Alessandro Rinaldi <ale@alerinaldi.it>"
# util-linux fornisce chrt, usato dal wrapper per priorita' RT FIFO
# (gia' di solito Essential ma lo dichiariamo esplicito per chiarezza).
DEPS="gambas3-runtime, gambas3-gb-qt6, gambas3-gb-form, gambas3-gb-net, gambas3-gb-net-curl, util-linux, openssl"

declare -A DESCRIPTIONS=(
  [brrm]="BoxRally Race Manager - cronometraggio postazione arrivo per gare Soap Box"
  [brrm-partenza]="BoxRally Race Manager - cronometraggio postazione partenza per gare Soap Box"
)

declare -A DESKTOP_NAMES=(
  [brrm]="BRRM"
  [brrm-partenza]="BRRM Partenza"
)

for p in brrm brrm-partenza; do
  ver=$(cat "$p/.version")
  # Se VERSION_SUFFIX è settato (es. da CI come "YYYYMMDD.shortsha"),
  # lo appende alla versione base con il separatore + (compatibile Debian).
  if [ -n "${VERSION_SUFFIX:-}" ]; then
    ver="${ver}+${VERSION_SUFFIX}"
  fi
  desc=${DESCRIPTIONS[$p]}
  stage=$(mktemp -d)
  chmod 755 "$stage"
  trap 'rm -rf "$stage"' EXIT

  # Archivio Gambas -> /usr/lib/<pkg>/<pkg>.gambas (non in PATH, chiamato dal wrapper)
  install -Dm755 "$p/$p.gambas" "$stage/usr/lib/$p/$p.gambas"

  # Shell wrapper in PATH: tenta SCHED_FIFO via chrt, fallback a priorita'
  # normale se i limiti utente non lo consentono (caso tipico: primo lancio
  # appena installato, prima del logout/login successivo).
  install -d "$stage/usr/bin"
  cat > "$stage/usr/bin/$p" <<EOF
#!/bin/sh
# brrm wrapper: usa priorita' RT FIFO se rtprio limit lo permette.
if chrt -f 50 /bin/true 2>/dev/null; then
  exec chrt -f 50 /usr/bin/gbr3 /usr/lib/$p/$p.gambas "\$@"
else
  exec /usr/bin/gbr3 /usr/lib/$p/$p.gambas "\$@"
fi
EOF
  chmod 755 "$stage/usr/bin/$p"

  # Icona
  install -Dm644 "$p/.icon.png" "$stage/usr/share/icons/hicolor/256x256/apps/$p.png"

  # .desktop
  install -d "$stage/usr/share/applications"
  cat > "$stage/usr/share/applications/$p.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=${DESKTOP_NAMES[$p]}
Comment=$desc
Exec=$p
Icon=$p
Categories=Utility;
Terminal=false
EOF

  # DEBIAN/control
  install -d "$stage/DEBIAN"
  cat > "$stage/DEBIAN/control" <<EOF
Package: $p
Version: $ver
Section: utils
Priority: optional
Architecture: all
Depends: $DEPS
Maintainer: $MAINTAINER
Description: $desc
EOF

  # /etc/security/limits.d: concede rtprio 50 a tutti gli utenti, cosi'
  # il wrapper puo' usare chrt -f 50 senza root. Re-login richiesto la
  # prima volta dopo l'install per applicare il limite alla sessione.
  install -d "$stage/etc/security/limits.d"
  cat > "$stage/etc/security/limits.d/20-$p.conf" <<EOF
# Permette agli utenti di impostare priorita' RT fino a 50.
# Installato da $p per ridurre il jitter scheduler nel timing fotocellule.
*  -  rtprio  50
EOF
  chmod 644 "$stage/etc/security/limits.d/20-$p.conf"

  out="dist/${p}_${ver}_all.deb"
  fakeroot dpkg-deb --build "$stage" "$out" >/dev/null
  rm -rf "$stage"
  trap - EXIT

  echo "Created: $out"
done

echo
echo "Pacchetti pronti in dist/:"
ls -la dist/*.deb

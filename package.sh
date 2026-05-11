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
DEPS="gambas3-runtime, gambas3-gb-qt6, gambas3-gb-form, gambas3-gb-net"

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

  # Eseguibile -> /usr/bin/<nome>
  install -Dm755 "$p/$p.gambas" "$stage/usr/bin/$p"

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

  out="dist/${p}_${ver}_all.deb"
  fakeroot dpkg-deb --build "$stage" "$out" >/dev/null
  rm -rf "$stage"
  trap - EXIT

  echo "Created: $out"
done

echo
echo "Pacchetti pronti in dist/:"
ls -la dist/*.deb

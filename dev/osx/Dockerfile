# Container per eseguire brrm / brrm-partenza dal Mac via XQuartz.
# Mirroring delle dipendenze della CI (debian:trixie + Gambas 3 + Qt6) +
# librerie X11 client-side per il forwarding verso XQuartz sull'host.
FROM debian:trixie

RUN apt-get update && apt-get install -y --no-install-recommends \
      gambas3-runtime \
      gambas3-gb-qt6 \
      gambas3-gb-qt6-x11 \
      gambas3-gb-form \
      gambas3-gb-net \
      gambas3-gb-net-curl \
      gambas3-devel \
      libqt6gui6 \
      libxkbcommon-x11-0 \
      libdbus-1-3 \
      ca-certificates \
      fonts-dejavu-core \
      locales \
 && sed -i 's/# en_US.UTF-8/en_US.UTF-8/' /etc/locale.gen \
 && locale-gen \
 && rm -rf /var/lib/apt/lists/*
ENV LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8

# L'utente boxrally mappa quello del Mac (uid 501) per permessi sui volumi
# mounted. UID/GID si possono override a build time.
ARG UID=501
ARG GID=20
RUN groupadd -g "$GID" -o boxrally 2>/dev/null || true \
 && useradd -u "$UID" -g "$GID" -m -s /bin/bash boxrally
USER boxrally
WORKDIR /workspace

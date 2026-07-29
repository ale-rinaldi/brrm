# Container Linux per provare la FINESTRA nativa del finto cronometro.
#
# Stesso schema del dev/osx del legacy — Xvfb dentro il container, x11vnc per
# guardarlo da Screen Sharing del Mac — ma con Go e gli header X11 al posto di
# Gambas e Qt.
#
# Serve perché Gio ha bisogno di una sessione grafica vera: sul Mac di sviluppo,
# lanciato da un terminale senza accesso al window server, va in panico dentro la
# creazione della finestra. Qui l'X server ce l'ha, ed è lo stesso trucco che
# risolveva le modali nere di Qt6.
FROM golang:1.26-trixie

RUN apt-get update && apt-get install -y --no-install-recommends \
      libx11-dev libx11-xcb-dev libxcb1-dev libxkbcommon-dev libxkbcommon-x11-dev \
      libwayland-dev libxcursor-dev libxfixes-dev libxi-dev \
      libgles2-mesa-dev libegl1-mesa-dev libvulkan-dev \
      pkg-config \
      xvfb x11vnc fluxbox x11-utils xdotool scrot imagemagick \
      fonts-dejavu-core ca-certificates \
 && rm -rf /var/lib/apt/lists/*

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod 755 /usr/local/bin/entrypoint.sh
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
WORKDIR /src

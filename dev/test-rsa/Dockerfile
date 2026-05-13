# Image per testare il branch feature/gb-openssl-native-jwt prima che
# gb.openssl 3.21.99+ (con classi PKey/Signature) arrivi nei repo Debian.
#
# Parte da brrm-dev (trixie + gambas3 runtime), aggiunge build-deps di
# gambas3, clona il nostro fork al branch della MR upstream e ricompila
# l'intero stack. Risultato: interprete + componenti (qt6, form, ecc.) +
# gb.openssl-con-RSA buildati dallo stesso sorgente coerente. Niente
# overlay di .so su runtime estraneo, niente ABI-mismatch.
#
# Una volta che la MR upstream e' merged e Debian aggiorna il pacchetto,
# brrm-dev:latest sara' sufficiente e quest'image diventa obsoleta.
FROM brrm-dev:latest
USER root

RUN echo 'deb-src http://deb.debian.org/debian trixie main' \
      > /etc/apt/sources.list.d/source.list \
 && apt-get update \
 && apt-get build-dep -y gambas3 \
 && apt-get install -y --no-install-recommends \
      build-essential automake autoconf libtool pkg-config dpkg-dev sudo git ca-certificates \
 && echo "boxrally ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/boxrally \
 && rm -rf /var/lib/apt/lists/*

# Repo da clonare e branch sono override-ablili a build time (utili se
# la PR si sposta di owner o cambia nome).
ARG GAMBAS_REPO=https://gitlab.com/alerinaldi/gambas.git
ARG GAMBAS_BRANCH=feature/gb-openssl-rsa-signature

WORKDIR /build
RUN git clone --depth 1 --branch "$GAMBAS_BRANCH" "$GAMBAS_REPO" gambas \
 && cd gambas && git log -1 --oneline

WORKDIR /build/gambas
RUN ./reconf-all 2>&1 | tail -3
RUN ./configure -C --disable-keyring --disable-sqlite2 --disable-qt4 2>&1 | tail -10
RUN make -j$(nproc) 2>&1 | tail -5
RUN make install 2>&1 | tail -5

USER boxrally
WORKDIR /workspace

# Image per eseguire brrm-test e brrm-partenza-test (unit test).
# Estende brrm-dev con i componenti Gambas che il framework gb.test
# si aspetta a runtime (gb.pcre per il formatting delle assertion).
FROM brrm-dev:latest
USER root

RUN apt-get update -q \
 && apt-get install -y --no-install-recommends gambas3-gb-pcre \
 && rm -rf /var/lib/apt/lists/*

USER boxrally
WORKDIR /workspace

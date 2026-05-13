#!/usr/bin/env bash
# Esegue pytest per server/ in una venv usa-e-getta dentro /tmp.
# La venv NON viene scritta nel project tree per non sporcare. Per CI
# basta avere python3 + pip nel PATH.
set -euo pipefail
cd "$(dirname "$0")/.."

VENV=/tmp/brrm-server-test-venv
if [ ! -d "$VENV" ]; then
  python3 -m venv "$VENV"
  "$VENV/bin/pip" install --quiet --upgrade pip
  "$VENV/bin/pip" install --quiet -r server/requirements.txt
  "$VENV/bin/pip" install --quiet pytest httpx
fi

# Assicura BRRM_ALIGN_PASSWORD per il side-effect di import del modulo.
export BRRM_ALIGN_PASSWORD="${BRRM_ALIGN_PASSWORD:-test}"

PYTHONPATH=server "$VENV/bin/pytest" -v server/tests/ "$@"

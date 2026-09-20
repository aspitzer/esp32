#!/usr/bin/env bash
# Envoltorio para launchd: launchd no lee .env, asi que lo cargamos aqui.
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/broker"
set -a; . ./.env; set +a

# launchd arranca con un PATH minimo que no incluye /opt/homebrew/bin. Sin
# esto, tmux y claude no se encuentran y las acciones de la fase 4 fallan
# SOLO cuando corre como servicio, que es como corre de verdad.
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
: "${TMUX_BIN:=$(command -v tmux   || echo tmux)}"
: "${CLAUDE_BIN:=$(command -v claude || echo claude)}"
export TMUX_BIN CLAUDE_BIN

# ACTIONS_DEBUG=1 en .env registra que paneles ve tmux y contra que compara.
# Es lo que hizo falta para ver que los campos llegaban pegados.

exec /opt/homebrew/bin/bun src/index.ts

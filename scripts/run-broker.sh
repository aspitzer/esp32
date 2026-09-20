#!/usr/bin/env bash
# Envoltorio para launchd: launchd no lee .env, asi que lo cargamos aqui.
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/broker"
set -a; . ./.env; set +a
exec /opt/homebrew/bin/bun src/index.ts

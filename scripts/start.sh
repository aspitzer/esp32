#!/usr/bin/env bash
# Arranca Mosquitto y el broker. Idempotente: no duplica procesos.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/broker"

[ -f .env ] || { echo "falta broker/.env (copia .env.example y rellena)"; exit 1; }

MOSQ="${MOSQUITTO_BIN:-/opt/homebrew/opt/mosquitto/sbin/mosquitto}"

if pgrep -f "mosquitto -c mosquitto/mosquitto.conf" > /dev/null; then
  echo "mosquitto ya corriendo"
else
  "$MOSQ" -c mosquitto/mosquitto.conf > /tmp/cad-mosquitto.log 2>&1 &
  sleep 1
  echo "mosquitto arrancado (log: /tmp/cad-mosquitto.log)"
fi

if pgrep -f "bun src/index.ts" > /dev/null; then
  echo "broker ya corriendo"
else
  set -a; . ./.env; set +a
  bun src/index.ts > /tmp/cad-broker.log 2>&1 &
  sleep 3
  echo "broker arrancado (log: /tmp/cad-broker.log)"
fi

curl -s "http://127.0.0.1:${HOOK_PORT:-8787}/health" | python3 -m json.tool | head -20 || true

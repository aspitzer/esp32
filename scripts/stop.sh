#!/usr/bin/env bash
# Para el broker y el mock. Mosquitto se deja vivo: el estado retained sobrevive.
pkill -f "bun src/index.ts"       && echo "broker parado"      || echo "broker no estaba"
pkill -INT -f "mock-agent.ts"     && echo "mock parado"        || echo "mock no estaba"

#!/usr/bin/env bash
# Instala Mosquitto y el broker como LaunchAgents del usuario: arrancan al
# iniciar sesion y launchd los resucita si mueren.
#
# Con el hook de permisos activo esto deja de ser opcional: si el broker no
# esta, cada evento de Claude Code intenta un POST que falla.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AGENTS="$HOME/Library/LaunchAgents"
MOSQ="${MOSQUITTO_BIN:-/opt/homebrew/opt/mosquitto/sbin/mosquitto}"
UID_N="$(id -u)"

[ -x "$MOSQ" ]            || { echo "no encuentro mosquitto en $MOSQ"; exit 1; }
[ -f "$ROOT/broker/.env" ] || { echo "falta broker/.env"; exit 1; }

mkdir -p "$AGENTS" "$HOME/Library/Logs"

cat > "$AGENTS/com.andres.cad-mosquitto.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.andres.cad-mosquitto</string>
  <key>ProgramArguments</key>
  <array>
    <string>$MOSQ</string><string>-c</string><string>mosquitto/mosquitto.conf</string>
  </array>
  <key>WorkingDirectory</key><string>$ROOT/broker</string>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>$HOME/Library/Logs/cad-mosquitto.log</string>
  <key>StandardErrorPath</key><string>$HOME/Library/Logs/cad-mosquitto.log</string>
</dict>
</plist>
EOF

cat > "$AGENTS/com.andres.cad-broker.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.andres.cad-broker</string>
  <key>ProgramArguments</key><array><string>$ROOT/scripts/run-broker.sh</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>10</integer>
  <key>StandardOutPath</key><string>$HOME/Library/Logs/cad-broker.log</string>
  <key>StandardErrorPath</key><string>$HOME/Library/Logs/cad-broker.log</string>
</dict>
</plist>
EOF

for L in com.andres.cad-mosquitto com.andres.cad-broker; do
  plutil -lint "$AGENTS/$L.plist" > /dev/null
  launchctl bootout "gui/$UID_N/$L" 2>/dev/null || true
  launchctl bootstrap "gui/$UID_N" "$AGENTS/$L.plist"
  echo "$L instalado"
done

sleep 6
curl -s -m 5 "http://127.0.0.1:${HOOK_PORT:-8787}/health" | python3 -m json.tool | head -8 \
  || echo "el broker aun no responde; mira ~/Library/Logs/cad-broker.log"

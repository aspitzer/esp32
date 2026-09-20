#!/usr/bin/env bash
# Quita los LaunchAgents. Los procesos mueren y no vuelven al reiniciar.
UID_N="$(id -u)"
for L in com.andres.cad-mosquitto com.andres.cad-broker; do
  launchctl bootout "gui/$UID_N/$L" 2>/dev/null && echo "$L parado" || echo "$L no estaba"
  rm -f "$HOME/Library/LaunchAgents/$L.plist"
done

#!/bin/bash
# agentes.sh [N] [cols]: N agents side by side on the ultrawide (5120x1392 work area), one class each, each in its own
# copy D:/SteamLibrary/steamapps/common/SacredA<i> (Steam's wrapper only runs copies under steamapps/common). Keeps
# them running: a copy that closed or crashed is started again in its own cell. Never saves.
S=/c/Users/silas/projects/sacred-bild/tools/entrenamiento
G="/d/SteamLibrary/steamapps/common"
N=${1:-8}
COLS=${2:-4}
ROWS=$(( (N + COLS - 1) / COLS ))
W=$(( 5120 / COLS ))
H=$(( 1392 / ROWS ))

setup() {    # $1 = index
  local i=$1 D="$G/SacredA$1"
  if [ ! -f "$D/Sacred.exe" ]; then
    bash "$S/make_instances.sh" "$i" >/dev/null
  fi
  # A1 runs the release only (the player, 2026-11-14: the build in his own folder); the others are the guinea pigs
  # and keep the build the coordinator installed in them (canary.sh), getting the release only if they have none.
  if [ "$i" = 1 ] || [ ! -f "$D/ddraw.dll" ]; then cp -p "$G/Sacred Gold/ddraw.dll" "$D/ddraw.dll" 2>/dev/null; fi
  echo -n 12320 > "$D/steam_appid.txt"
  python -I "$S/instance_ini.py" "$D/SacredBild.ini" "$i" "$W" "$H"
}

running() {  # pid of copy $1, or nothing
  powershell -NoProfile -Command "Get-Process Sacred -ErrorAction SilentlyContinue | Where-Object { \$_.Path -eq 'D:\SteamLibrary\steamapps\common\SacredA$1\Sacred.exe' } | Select-Object -First 1 -ExpandProperty Id" | tr -d '\r'
}

place() {    # back in its cell (a copy minimizes itself when another window takes the focus)
  local col=$(( ($1 - 1) % COLS )) row=$(( ($1 - 1) / COLS ))
  powershell -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "$(cygpath -w $S/place.ps1)" "SacredA$1" $(( col * W )) $(( row * H )) $W $H
}

start() {    # $1 = index
  local i=$1 col=$(( ($1 - 1) % COLS )) row=$(( ($1 - 1) / COLS ))
  powershell -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "$(cygpath -w $S/refocus.ps1)" save "$(cygpath -w $S/fg.txt)"
  powershell -c "Start-Process 'D:\SteamLibrary\steamapps\common\SacredA$i\Sacred.exe' -WorkingDirectory 'D:\SteamLibrary\steamapps\common\SacredA$i'"
  sleep 14
  place "$i"
  powershell -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "$(cygpath -w $S/refocus.ps1)" restore "$(cygpath -w $S/fg.txt)"
  echo "$(date +%T) a$i started at cell $col,$row"
}

for i in $(seq 1 "$N"); do setup "$i"; done
while true; do
  for i in $(seq 1 "$N"); do
    if [ -z "$(running "$i")" ]; then start "$i"; else place "$i"; fi
  done
  sleep 20
done

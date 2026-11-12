#!/bin/bash
# make_instances.sh i...: steamapps/common/SacredA<i>, each a game folder of its own. Files over 2 MB (paks, world, movies)
# are hard links to the original (no disk used, never written); everything small (configs, saves, logs, the
# ModKit's learning) is a real copy, so the instances never write over each other or over the player's game.
SRC="/d/SteamLibrary/steamapps/common/Sacred Gold"
DST=/d/SteamLibrary/steamapps/common
for i in "$@"; do
  D="$DST/SacredA$i"
  [ -f "$D/Sacred.exe" ] && { echo "a$i exists"; continue; }
  mkdir -p "$D"
  (cd "$SRC" && find . -type d ! -path "./_ultrawide_backup*" ! -path "./SacredBild-logs*" ! -path "./Capture*") | while read -r d; do mkdir -p "$D/$d"; done
  (cd "$SRC" && find . -type f ! -path "./_ultrawide_backup/*" ! -path "./SacredBild-logs/*" ! -path "./Capture/*" ! -name "*.dmp" ! -name "*.prev*.log" ! -name "Sacred.exe.backup") | while read -r f; do
    sz=$(stat -c %s "$SRC/$f")
    # bin/sgf.bin is big but rewritten by the game at each load (the quest scripts): a copy, never a link.
    if [ "$sz" -gt 2000000 ] && [ "$f" != "./ddraw.dll" ] && [ "$f" != "./bin/sgf.bin" ]; then
      powershell -NoProfile -Command "New-Item -ItemType HardLink -Path '$(cygpath -w "$D/$f")' -Target '$(cygpath -w "$SRC/$f")' | Out-Null" 2>/dev/null || ln "$SRC/$f" "$D/$f"
    else
      cp -p "$SRC/$f" "$D/$f"
    fi
  done
  echo "a$i made"
done

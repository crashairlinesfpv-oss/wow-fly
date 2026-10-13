#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OUT="$ROOT/bin"
REPO="$(cd "$ROOT/../.." && pwd)"

VELOCIDIR="${VelocidroneDir:-$HOME/Apps/velocidrone/app}"
MAN="${GameManagedDir:-$VELOCIDIR/velocidrone_Data/Managed}"

if [[ -f "$VELOCIDIR/BepInEx/core/BepInEx.dll" ]]; then
  BEP="$VELOCIDIR/BepInEx/core"
else
  BEP="${BepInExCoreDir:-$HOME/.local/share/Steam/steamapps/common/Liftoff/BepInEx/core}"
fi

mkdir -p "$OUT"

mcs -target:library -out:"$OUT/PoseBridge.dll" -sdk:4.7.2 \
  -r:"$BEP/BepInEx.dll" \
  -r:"$MAN/UnityEngine.dll" \
  -r:"$MAN/UnityEngine.CoreModule.dll" \
  -r:"$MAN/UnityEngine.PhysicsModule.dll" \
  -r:"$MAN/UnityEngine.InputLegacyModule.dll" \
  "$ROOT/PosePacket.cs" \
  "$ROOT/ContactApply.cs" \
  "$ROOT/OpenSky.cs" \
  "$ROOT/PoseSender.cs" \
  "$ROOT/PoseBridgePlugin.cs"

echo "Built $OUT/PoseBridge.dll"

mcs -target:exe -out:"$OUT/LayoutCheck.exe" -sdk:4.7.2 \
  "$ROOT/PosePacket.cs" \
  "$ROOT/LayoutCheck.cs"
mono "$OUT/LayoutCheck.exe" > "$OUT/layout.txt"
python3 "$REPO/tools/pose_packet.py"

PLUGINS="$VELOCIDIR/BepInEx/plugins"
if [[ -d "$PLUGINS" ]]; then
  cp -f "$OUT/PoseBridge.dll" "$PLUGINS/"
  echo "Installed -> $PLUGINS/PoseBridge.dll"
else
  echo "BepInEx plugins dir not found. Copy:"
  echo "  cp $OUT/PoseBridge.dll \$VELOCIDIR/BepInEx/plugins/"
  exit 1
fi

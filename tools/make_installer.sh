#!/usr/bin/env bash
# Make the Conker Redux installer from the last Windows build (tools/rebuild_all.sh), in WSL:
#   tools/make_installer.sh [version]          (default: 0.1.0)
# -> C:\dev\ConkerRecompWin\dist\ConkerRedux-Setup-<version>.exe, and the same files as a portable
#    zip, ConkerRedux-<version>-Windows-Portable.zip (unzip anywhere, run ConkerRedux.exe).
# Staged in C:\dev\ConkerRecompWin\installer\stage: the program (as ConkerRedux.exe), its DLLs and
# assets, the Visual C++ runtime next to it (from Visual Studio's redistributable files, so the
# player needn't install it), LICENSE.txt, README.txt and the source code (source\, as the GPL asks
# for when the program is passed on). No game data: the player chooses their own ROM.
# Needs Inno Setup 6 (winget install JRSoftware.InnoSetup).
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
W=${CONKER_WIN_DIR:-/mnt/c/dev/ConkerRecompWin}
B=$W/host/build
version=${1:-0.1.0}
. "$R/tools/release_license.sh" # license()

S=$W/installer/stage
rm -rf "$S"
mkdir -p "$S/source" "$W/dist"

cp "$B/ConkerBFDReloaded.exe" "$S/ConkerRedux.exe"
cp "$B/SDL2.dll" "$B/dxcompiler.dll" "$B/dxil.dll" "$S/"
cp -r "$R/host/assets" "$S/assets"

# The Visual C++ runtime, app-local (Microsoft's redistributable DLLs, from the newest Visual Studio).
crt=$(ls -d /mnt/c/Program\ Files/Microsoft\ Visual\ Studio/*/*/VC/Redist/MSVC/*/x64/Microsoft.VC14*.CRT 2>/dev/null | sort -V | tail -1)
[ -n "$crt" ] || { echo "no Visual C++ redistributable files found under Visual Studio"; exit 1; }
cp "$crt"/*.dll "$S/"

# The source this was built from: git's committed files (the ROM and anything made from it are
# never committed). Built from uncommitted changes, it says so.
if [ -n "$(git -C "$R" status --porcelain 2>/dev/null)" ]; then
  echo "warning: uncommitted changes in $R: the bundled source is the last commit, not this build"
fi
git -C "$R" archive --format=zip --prefix=ConkerRedux-source/ -o "$S/source/ConkerRedux-$version-source.zip" HEAD
commit=$(git -C "$R" rev-parse --short HEAD)

{
  printf 'Conker Redux %s is a modified version of Conker'"'"'s Bad Fur Day: Reloaded (built from\n' "$version"
  printf 'commit %s). Its complete source code is included with it, in the source folder, and is\n' "$commit"
  printf 'published at https://github.com/studioa2kiadev/ConkerBFDRedux.\n\n'
  license
} | sed 's/$/\r/' > "$S/LICENSE.txt"

sed 's/$/\r/' > "$S/README.txt" <<EOF
Conker Redux $version
==================

Conker's Bad Fur Day (N64) running natively on PC, with a modern analog camera, an Xbox
controller layout, 60 fps, widescreen and high resolution.

HOW TO PLAY
 1. Start Conker Redux from the Start menu or the desktop (portable zip: run ConkerRedux.exe).
 2. The first time, choose your own copy of the game: the US ROM of Conker's Bad Fur Day
    (.z64, .n64 or .v64). It's only asked for once. No game data is included.
    (A ROM put in the game's folder is found without asking.)
 3. Play. Press the View button (or Esc) for all the settings: graphics, controls, sound.

CONTROLS (Xbox controller)
 Left stick  move            A  jump              B  action / attack
 Right stick camera          R3 camera distance  RT  center camera / look
 LT          crouch          X, Y  rotate camera  D-pad up  first-person view
 L3          free camera off / on (off: the right stick is the N64's C-buttons)

Your settings, saves and ROM are kept in %LOCALAPPDATA%\ConkerRedux (the uninstaller
leaves them). If the game crashes, a report is written to crash.log there.
Hold Shift while starting it (or use "Conker Redux (launcher, mods)") for the launcher.

Needs Windows 10 or 11 (64-bit) and a graphics card with DirectX 12 or Vulkan.

Built on Conker's Bad Fur Day: Reloaded (dahmedvall95), CBFD-Recompiled (Sean Ciaschi), the
mkst/conker decompilation, N64Recomp / N64ModernRuntime / RecompFrontend and RT64. See
LICENSE.txt. Conker's Bad Fur Day is (c) Rare / Microsoft; this is a fan project, not
affiliated with them.
EOF

# Inno Setup's compiler (a per-user or a system-wide install).
local_appdata=$(wslpath "$(cmd.exe /c 'echo %LOCALAPPDATA%' 2>/dev/null | tr -d '\r')")
iscc=""
for candidate in "$local_appdata/Programs/Inno Setup 6/ISCC.exe" "/mnt/c/Program Files (x86)/Inno Setup 6/ISCC.exe" "/mnt/c/Program Files/Inno Setup 6/ISCC.exe"; do
  [ -f "$candidate" ] && { iscc=$candidate; break; }
done
[ -n "$iscc" ] || { echo "Inno Setup 6 not found: winget install JRSoftware.InnoSetup"; exit 1; }

"$iscc" /Q \
  "/DStageDir=$(wslpath -w "$S")" \
  "/DOutputDir=$(wslpath -w "$W/dist")" \
  "/DAppVersion=$version" \
  "/DIconFile=$(wslpath -w "$R/host/icon/conker.ico")" \
  "$(wslpath -w "$R/host/installer/ConkerRedux.iss")"
ls -la "$W/dist/ConkerRedux-Setup-$version.exe"

# The portable zip: the same files, in a ConkerRedux folder.
portable=$W/dist/ConkerRedux-$version-Windows-Portable.zip
rm -f "$portable"
(cd "$W/installer" && rm -rf ConkerRedux && cp -r stage ConkerRedux && cmd.exe /c "tar -a -cf $(wslpath -w "$portable") ConkerRedux" >/dev/null 2>&1; rm -rf ConkerRedux)
ls -la "$portable"

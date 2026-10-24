#!/bin/sh
# Copies the built mod into the game's Modloader folder. Run from the repo root.
# GAME=/path/to/GTA\ San\ Andreas ./deploy.sh for another install.
set -e
GAME="${GAME:-/mnt/d/Games/GTA San Andreas}"
DEST="$GAME/modloader/FPV Drone"
mkdir -p "$DEST"
# The loader is locked while the game runs; it only changes rarely.
cp build/GTASA.FPV.asi "$DEST/" 2>/dev/null || echo "loader in use, kept the running one"
# The running game hot-reloads the core within a second of this copy.
cp build/GTASA.FPV.core.dll "$DEST/"
# LIBFPV_DLL=~/Projects/libfpv/build/win32-kamikaze/libfpv.dll ./deploy.sh for a kamikaze build.
cp "${LIBFPV_DLL:-$HOME/Projects/libfpv/build/win32/libfpv.dll}" "$DEST/" 2>/dev/null || echo "libfpv.dll in use, kept the running one"
echo "deployed to $DEST"

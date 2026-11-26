#!/bin/sh
# Copies the built mod into the game's Modloader folder. Run from the repo root.
# GAME=/path/to/GTA\ San\ Andreas ./deploy.sh for another install.
# libfpv comes from build/win32 of the libfpv checkout (LIBFPV_DIR, default
# ~/Projects/libfpv), or build/win32-kamikaze on a kamikaze dev machine
# (LIBFPV_KAMIKAZE_DEV=1 or true); LIBFPV_DLL picks another one. The mod must
# be built for the same kind (GTASA_FPV_KAMIKAZE): fpv_frame differs.
set -e
GAME="${GAME:-/mnt/d/Games/GTA San Andreas}"
DEST="$GAME/modloader/FPV Drone"
LIBFPV="${LIBFPV_DIR:-$HOME/Projects/libfpv}"
case "$LIBFPV_KAMIKAZE_DEV" in 1|true) KAMIKAZE=ON; LIB_DEFAULT="$LIBFPV/build/win32-kamikaze/libfpv.dll" ;;
  *) KAMIKAZE=OFF; LIB_DEFAULT="$LIBFPV/build/win32/libfpv.dll" ;; esac
LIB="${LIBFPV_DLL:-$LIB_DEFAULT}"
[ -f "$LIB" ] || { echo "missing $LIB: build libfpv first" >&2; exit 1; }
MOD_KAMIKAZE=$(sed -n 's/^GTASA_FPV_KAMIKAZE:BOOL=//p' build/CMakeCache.txt)
LIB_KAMIKAZE=OFF
grep -q fpv_kamikaze_poll_detonation "$LIB" && LIB_KAMIKAZE=ON
[ "$MOD_KAMIKAZE" = "$LIB_KAMIKAZE" ] || {
  echo "the mod is built with GTASA_FPV_KAMIKAZE=${MOD_KAMIKAZE:-<unset>} but $LIB is kamikaze=$LIB_KAMIKAZE:" >&2
  echo "  cmake -S . -B build -DGTASA_FPV_KAMIKAZE=$LIB_KAMIKAZE && cmake --build build" >&2
  exit 1; }
[ "$LIB_KAMIKAZE" = "$KAMIKAZE" ] || [ -n "$LIBFPV_DLL" ] || echo "note: LIBFPV_KAMIKAZE_DEV says kamikaze=$KAMIKAZE"
mkdir -p "$DEST"
# The loader is locked while the game runs; it only changes rarely.
cp build/GTASA.FPV.asi "$DEST/" 2>/dev/null || echo "loader in use, kept the running one"
# The running game hot-reloads the core within a second of this copy.
cp build/GTASA.FPV.core.dll "$DEST/"
cp "$LIB" "$DEST/" 2>/dev/null || echo "libfpv.dll in use, kept the running one (restart the game for a new libfpv)"
echo "deployed to $DEST (libfpv kamikaze=$LIB_KAMIKAZE from $LIB)"

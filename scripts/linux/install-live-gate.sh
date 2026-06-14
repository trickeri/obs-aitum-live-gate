#!/usr/bin/env bash
set -euo pipefail

PLUGIN_NAME="obs-aitum-live-gate"
CONFIG="RelWithDebInfo"
INSTALL_SCOPE="user"
BUILD_DIR=""

usage() {
  cat <<USAGE
Usage: scripts/linux/install-live-gate.sh [--build-dir DIR] [--config RelWithDebInfo] [--global]

Installs the built OBS Aitum Live Gate Linux plugin.
Default target is the per-user OBS plugin folder:
  ~/.config/obs-studio/plugins/obs-aitum-live-gate

Set OBS_USER_PLUGIN_ROOT for Flatpak/custom OBS config roots, for example:
  OBS_USER_PLUGIN_ROOT="$HOME/.var/app/com.obsproject.Studio/config/obs-studio/plugins"

Use --global to install to Arch/system OBS paths:
  /usr/lib/obs-plugins
  /usr/share/obs/obs-plugins/obs-aitum-live-gate
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD_DIR="${2:?missing build dir}"; shift 2 ;;
    --config) CONFIG="${2:?missing config}"; shift 2 ;;
    --global) INSTALL_SCOPE="global"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "$BUILD_DIR" ]]; then
  for candidate in build_linux_x86_64 build_x86_64; do
    if [[ -d "$candidate" ]]; then
      BUILD_DIR="$candidate"
      break
    fi
  done
fi

if [[ -z "$BUILD_DIR" || ! -d "$BUILD_DIR" ]]; then
  cat >&2 <<ERR
Build directory not found.
Build first, for example:
  cmake --preset linux-x86_64
  cmake --build --preset linux-x86_64 --config $CONFIG
ERR
  exit 1
fi

SO_PATH="$(find "$BUILD_DIR" -type f \( -name "${PLUGIN_NAME}.so" -o -name "lib${PLUGIN_NAME}.so" \) | head -n 1)"
if [[ -z "$SO_PATH" ]]; then
  echo "Could not find built ${PLUGIN_NAME}.so under $BUILD_DIR" >&2
  exit 1
fi

if [[ "$INSTALL_SCOPE" == "global" ]]; then
  BIN_DIR="/usr/lib/obs-plugins"
  DATA_DIR="/usr/share/obs/obs-plugins/${PLUGIN_NAME}"
  SUDO="sudo"
else
  USER_PLUGIN_ROOT="${OBS_USER_PLUGIN_ROOT:-${HOME}/.config/obs-studio/plugins}"
  BIN_DIR="${USER_PLUGIN_ROOT}/${PLUGIN_NAME}/bin/64bit"
  DATA_DIR="${USER_PLUGIN_ROOT}/${PLUGIN_NAME}/data"
  SUDO=""
fi

$SUDO mkdir -p "$BIN_DIR" "$DATA_DIR/locale"
$SUDO install -m 755 "$SO_PATH" "$BIN_DIR/${PLUGIN_NAME}.so"
$SUDO install -m 644 "data/locale/en-US.ini" "$DATA_DIR/locale/en-US.ini"

cat <<OK
Installed ${PLUGIN_NAME}:
  binary: $BIN_DIR/${PLUGIN_NAME}.so
  data:   $DATA_DIR

Restart OBS Studio, then use:
  Tools -> Preview Go Live Gate
OK
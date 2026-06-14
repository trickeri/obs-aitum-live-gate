#!/usr/bin/env bash
set -euo pipefail

VERSION="1.0.8"
INSTALL_SCOPE="user"
WORK_DIR="${TMPDIR:-/tmp}/aitum-multistream-${VERSION}"
DEB_NAME="aitum-multistream-linux-gnu.deb"
URL="https://github.com/Aitum/obs-aitum-multistream/releases/download/${VERSION}/${DEB_NAME}"
EXPECTED_SHA256="08f0e869be80a1c44ca265a3fadfac033c3f4c8d4c7fb1f8d28838b9554d9c6d"

usage() {
  cat <<USAGE
Usage: scripts/linux/install-aitum-arch.sh [--version VERSION] [--global]

Downloads Aitum Multistream's Linux .deb release and extracts it for OBS on Arch.
Default target is per-user:
  ~/.config/obs-studio/plugins/aitum-multistream

Set OBS_USER_PLUGIN_ROOT for Flatpak/custom OBS config roots, for example:
  OBS_USER_PLUGIN_ROOT="$HOME/.var/app/com.obsproject.Studio/config/obs-studio/plugins"

Use --global for Arch/system OBS paths:
  /usr/lib/obs-plugins
  /usr/share/obs/obs-plugins/aitum-multistream
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) VERSION="${2:?missing version}"; DEB_NAME="aitum-multistream-linux-gnu.deb"; URL="https://github.com/Aitum/obs-aitum-multistream/releases/download/${VERSION}/${DEB_NAME}"; EXPECTED_SHA256=""; shift 2 ;;
    --global) INSTALL_SCOPE="global"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

mkdir -p "$WORK_DIR"
cd "$WORK_DIR"

if [[ ! -f "$DEB_NAME" ]]; then
  echo "Downloading $URL"
  curl -L --fail --output "$DEB_NAME" "$URL"
fi

if [[ -n "$EXPECTED_SHA256" ]]; then
  ACTUAL="$(sha256sum "$DEB_NAME" | awk '{print $1}')"
  if [[ "$ACTUAL" != "$EXPECTED_SHA256" ]]; then
    echo "SHA-256 mismatch for $DEB_NAME" >&2
    echo "expected: $EXPECTED_SHA256" >&2
    echo "actual:   $ACTUAL" >&2
    exit 1
  fi
fi

rm -rf extract data-root
mkdir -p extract data-root

if command -v bsdtar >/dev/null 2>&1; then
  bsdtar -xf "$DEB_NAME" -C extract
else
  ar x "$DEB_NAME" --output extract
fi

DATA_TAR="$(find extract -maxdepth 1 -type f -name 'data.tar.*' | head -n 1)"
if [[ -z "$DATA_TAR" ]]; then
  echo "Could not find data.tar.* inside $DEB_NAME" >&2
  exit 1
fi

tar -xf "$DATA_TAR" -C data-root

SO_SRC="data-root/usr/lib/x86_64-linux-gnu/obs-plugins/aitum-multistream.so"
DATA_SRC="data-root/usr/share/obs/obs-plugins/aitum-multistream"
if [[ ! -f "$SO_SRC" || ! -d "$DATA_SRC" ]]; then
  echo "Aitum package layout was not recognized" >&2
  exit 1
fi

if [[ "$INSTALL_SCOPE" == "global" ]]; then
  BIN_DIR="/usr/lib/obs-plugins"
  DATA_DIR="/usr/share/obs/obs-plugins/aitum-multistream"
  SUDO="sudo"
else
  USER_PLUGIN_ROOT="${OBS_USER_PLUGIN_ROOT:-${HOME}/.config/obs-studio/plugins}"
  BIN_DIR="${USER_PLUGIN_ROOT}/aitum-multistream/bin/64bit"
  DATA_DIR="${USER_PLUGIN_ROOT}/aitum-multistream/data"
  SUDO=""
fi

$SUDO mkdir -p "$BIN_DIR" "$DATA_DIR"
$SUDO install -m 755 "$SO_SRC" "$BIN_DIR/aitum-multistream.so"
$SUDO cp -a "$DATA_SRC/." "$DATA_DIR/"

cat <<OK
Installed Aitum Multistream ${VERSION}:
  binary: $BIN_DIR/aitum-multistream.so
  data:   $DATA_DIR

Restart OBS Studio, then check Docks for Aitum Multistream.
OK

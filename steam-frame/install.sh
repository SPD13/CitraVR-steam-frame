#!/usr/bin/env bash
# install.sh - installs or updates CitraVR - Steam Frame Edition on a Steam Frame. Runs ON the
# headset, as its user (steamos), for example from a computer:
#
#   ssh steamos@frame.local "curl -fsSL https://github.com/SPD13/CitraVR-steam-frame/releases/latest/download/install.sh | bash"
#
# It downloads the release's APK and files, checks them against the release's SHA256SUMS, adds
# CitraVR to the Steam library ("Citra VR - Steam Frame Edition", launched through Lepton) with
# its artwork. Running it again updates CitraVR; its folders, games and saves are kept.
#
#   install.sh                       latest release
#   install.sh --version v1.0.0      a given release
#   install.sh --uninstall           remove CitraVR from the library (its data stays)
#   install.sh --apk FILE --files DIR   local files instead of a release (register-frame.sh)
set -euo pipefail

REPO=SPD13/CitraVR-steam-frame
GAMEID=citravr
NAME='Citra VR - Steam Frame Edition'
APK_ASSET=CitraVR-steam-frame.apk
FILES_ASSET=citravr-steam-frame-files.tar.gz
DATA_DIR="$HOME/.local/share/$GAMEID"

say() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

version=latest
apk=
files=
uninstall=false
while [ $# -gt 0 ]; do
  case "$1" in
    --version) version=${2:?}; shift 2 ;;
    --apk) apk=${2:?}; shift 2 ;;
    --files) files=${2:?}; shift 2 ;;
    --uninstall) uninstall=true; shift ;;
    -h|--help) sed -n '2,16p' "$0" 2>/dev/null || true; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

[ -e "$HOME/.steam/steam.pipe" ] \
  || die "this doesn't look like a Steam Frame (no ~/.steam/steam.pipe). Run it on the headset, e.g. over ssh."

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if [ -z "$apk" ] && ! $uninstall; then
  if [ "$version" = latest ]; then
    url="https://github.com/$REPO/releases/latest/download"
  else
    url="https://github.com/$REPO/releases/download/$version"
  fi
  say "Downloading CitraVR ($version)..."
  for asset in SHA256SUMS "$APK_ASSET" "$FILES_ASSET"; do
    curl -fsSL --retry 3 -o "$work/$asset" "$url/$asset" || die "could not download $url/$asset"
  done
  (cd "$work" && sha256sum --check --quiet SHA256SUMS) || die "the download doesn't match SHA256SUMS"
  apk="$work/$APK_ASSET"
  mkdir -p "$work/files"
  tar -xzf "$work/$FILES_ASSET" -C "$work/files"
  files="$work/files"
fi
if ! $uninstall; then
  [ -f "$apk" ] || die "APK not found: $apk"
  [ -d "$files" ] || die "files directory not found: $files"
fi

if $uninstall; then
  if [ -z "$files" ]; then
    url="https://github.com/$REPO/releases/latest/download"
    curl -fsSL --retry 3 -o "$work/$FILES_ASSET" "$url/$FILES_ASSET" || die "could not download $FILES_ASSET"
    mkdir -p "$work/files" && tar -xzf "$work/$FILES_ASSET" -C "$work/files"
    files="$work/files"
  fi
  python3 "$files/install-library-art.py" --remove "$GAMEID" || true
  bash "$files/frame-register.sh" --uninstall
  rm -rf "$DATA_DIR"
  say "CitraVR is removed from the library. Its data (folders, saves) stays on the headset."
  exit 0
fi

# Add (or update) the title; Lepton installs the new APK when it's next launched.
mkdir -p "$HOME/devkit-game/$GAMEID" "$DATA_DIR"
cp "$apk" "$HOME/devkit-game/$GAMEID/CitraVR.apk"
bash "$files/frame-register.sh" "$HOME/devkit-game/$GAMEID/CitraVR.apk"

# Artwork, and the library name (Steam registers devkit titles as "Devkit Game: <gameid>").
rm -rf "$DATA_DIR/library-art"
cp -r "$files/library-art" "$DATA_DIR/library-art"
cp "$files/install-library-art.py" "$files/steam-library.py" "$DATA_DIR/"
python3 "$DATA_DIR/install-library-art.py" "$GAMEID" "$DATA_DIR/library-art" \
  || say "note: the artwork could not be installed; CitraVR works without it"
python3 "$DATA_DIR/steam-library.py" --gameid "$GAMEID" --name "$NAME" \
  --artwork-dir "$DATA_DIR/library-art" \
  || say "note: Steam's client did not answer; the title keeps the name 'Devkit Game: $GAMEID'"

say "Done: '$NAME' is in your Steam library. If CitraVR is running, quit it and start it again to update."

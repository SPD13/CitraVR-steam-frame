#!/usr/bin/env bash
# Installs a CitraVR build from this repository on a Steam Frame, for development: copies the
# APK and steam-frame/ files to the headset and runs steam-frame/install.sh there, which adds it
# to the Steam library ("Citra VR - Steam Frame Edition", launched through Lepton) with its
# artwork. To install a release instead, see the README (no build needed).
# Needs Developer Mode and ssh access as steamos (your key paired with the Frame, or its
# Developer Mode password). FRAME_HOST (default: frame.local) selects the headset.
# Usage: ./register-frame.sh [path/to/apk]      ./register-frame.sh --uninstall
set -euo pipefail
cd "$(dirname "$0")"
target="steamos@${FRAME_HOST:-frame.local}"
staging=/tmp/citravr-install

files=(steam-frame/install.sh steam-frame/frame-register.sh steam-frame/install-library-art.py
       steam-frame/steam-library.py)
ssh "$target" "rm -rf $staging && mkdir -p $staging/files"
scp -q "${files[@]}" "$target:$staging/files/"
scp -q -r steam-frame/library-art "$target:$staging/files/"
if [[ ${1:-} == --uninstall ]]; then
  ssh "$target" "bash $staging/files/install.sh --uninstall --files $staging/files; rm -rf $staging"
  exit
fi
apk=${1:-src/android/app/build/outputs/apk/release/app-release.apk}
[[ -f $apk ]] || { echo "APK missing: build it first (cd src/android && ./gradlew assembleRelease)." >&2; exit 1; }
scp "$apk" "$target:$staging/CitraVR.apk"
ssh "$target" "bash $staging/files/install.sh --apk $staging/CitraVR.apk --files $staging/files; rm -rf $staging"

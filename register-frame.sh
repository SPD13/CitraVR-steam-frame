#!/usr/bin/env bash
# Add CitraVR to the Steam Frame's Steam library (Non-Steam > Citra VR - Steam Frame Edition), launched through
# Lepton like a Title Upload with runtime "Android". Needs Developer Mode and ssh access as
# steamos (your key paired with the Frame, or its Developer Mode password).
# FRAME_HOST (default: frame.local) selects the headset.
# Also installs the library artwork in steam-frame/library-art (steam-frame/make_library_art.py
# makes it). Adapted from the Time Crisis VR Steam Frame port.
# Usage: ./register-frame.sh [path/to/apk]      ./register-frame.sh --uninstall
set -euo pipefail
cd "$(dirname "$0")"
target="steamos@${FRAME_HOST:-frame.local}"
gameid=citravr
remote_dir=".local/share/$gameid"
if [[ ${1:-} == --uninstall ]]; then
  scp -q steam-frame/frame-register.sh steam-frame/install-library-art.py "$target:/tmp/"
  ssh "$target" "python3 /tmp/install-library-art.py --remove $gameid || true; bash /tmp/frame-register.sh --uninstall; rm -rf /tmp/frame-register.sh /tmp/install-library-art.py ~/$remote_dir"
  exit
fi
apk=${1:-src/android/app/build/outputs/apk/release/app-release.apk}
[[ -f $apk ]] || { echo "APK missing: build it first (cd src/android && ./gradlew assembleRelease)." >&2; exit 1; }
ssh "$target" "mkdir -p ~/devkit-game/$gameid ~/$remote_dir"
scp -q steam-frame/frame-register.sh "$target:/tmp/frame-register.sh"
scp "$apk" "$target:devkit-game/$gameid/CitraVR.apk"
ssh "$target" "bash /tmp/frame-register.sh ~/devkit-game/$gameid/CitraVR.apk; rm -f /tmp/frame-register.sh"
if [[ -d steam-frame/library-art ]]; then
  ssh "$target" "mkdir -p ~/$remote_dir/library-art"
  scp -q steam-frame/library-art/*.png "$target:$remote_dir/library-art/"
  scp -q steam-frame/install-library-art.py "$target:$remote_dir/install-library-art.py"
  ssh "$target" "python3 ~/$remote_dir/install-library-art.py $gameid ~/$remote_dir/library-art" \
    || echo "note: the artwork could not be installed; CitraVR works without it"
fi
# Steam registers devkit titles as "Devkit Game: <gameid>": rename it and apply the artwork now,
# through the Steam client (the grid files above keep the artwork if this is unavailable).
scp -q steam-frame/steam-library.py "$target:$remote_dir/steam-library.py"
art=(); [[ -d steam-frame/library-art ]] && art=(--artwork-dir "~/$remote_dir/library-art")
ssh "$target" "python3 ~/$remote_dir/steam-library.py --gameid $gameid --name 'Citra VR - Steam Frame Edition' ${art[*]}" \
  || echo "note: Steam's client did not answer; the title keeps the name 'Devkit Game: $gameid'"

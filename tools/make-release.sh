#!/usr/bin/env bash
# Builds a release of CitraVR - Steam Frame Edition, signed with the project's release key, and
# with --publish, publishes it on GitHub, where steam-frame/install.sh downloads it from.
#
#   tools/make-release.sh v1.0.0             build and package into build/release/v1.0.0
#   tools/make-release.sh v1.0.0 --publish   same, then push the tag and create the release
#
# The release also gets the installer app for Windows, macOS and Linux (tools/make-installer.sh,
# which needs Go and a Mac).
#
# Needs JAVA_HOME and ANDROID_HOME (as for any build), gh (for --publish), and the release key:
# the keystore at ~/.config/citravr-steam-frame/release.keystore (or CITRAVR_KEYSTORE) and its
# password in the macOS Keychain item "citravr-steam-frame-release-key" (or
# CITRAVR_KEYSTORE_PASS). Every release must be signed with this same key, or it won't install
# over the previous one: keep it backed up (see the README's "Releases" section).
set -euo pipefail
cd "$(dirname "$0")/.."

REPO=SPD13/CitraVR-steam-frame
KEY_ALIAS=citravr-steam-frame
# SHA-256 of the release certificate, to check that the APK is signed with the release key.
CERT_SHA256=08afdaf9c8b20e4d9f01cf1ad4780d2a28e2dd3669b629ebc6e86af5e8c39884

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

version=${1:-}
[[ $version =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "usage: tools/make-release.sh vX.Y.Z [--publish]"
publish=false
[[ ${2:-} == --publish ]] && publish=true
[[ -z $(git status --porcelain) ]] || die "commit or stash your changes first"
: "${JAVA_HOME:?set JAVA_HOME}" "${ANDROID_HOME:?set ANDROID_HOME}"

keystore=${CITRAVR_KEYSTORE:-$HOME/.config/citravr-steam-frame/release.keystore}
[[ -f $keystore ]] || die "release keystore not found: $keystore"
pass=${CITRAVR_KEYSTORE_PASS:-$(security find-generic-password -s citravr-steam-frame-release-key -w 2>/dev/null)} \
  || die "release keystore password not found (Keychain item citravr-steam-frame-release-key)"

# The build names itself after the latest tag (versionName), so tag first.
if ! git rev-parse -q --verify "refs/tags/$version" >/dev/null; then
  git tag -a "$version" -m "CitraVR - Steam Frame Edition $version"
fi
[[ $(git rev-parse "$version^{commit}") == $(git rev-parse HEAD) ]] \
  || die "tag $version doesn't point at HEAD"

(cd src/android && ANDROID_KEYSTORE_FILE="$keystore" ANDROID_KEY_ALIAS=$KEY_ALIAS \
  ANDROID_KEYSTORE_PASS="$pass" ./gradlew --no-daemon assembleRelease)
apk=src/android/app/build/outputs/apk/release/app-release.apk

apksigner=$(ls "$ANDROID_HOME"/build-tools/*/apksigner | sort -V | tail -1)
"$apksigner" verify --print-certs "$apk" | grep -qi "certificate SHA-256 digest: $CERT_SHA256" \
  || die "the APK isn't signed with the release key"

out=build/release/$version
rm -rf "$out"
mkdir -p "$out/files"
cp "$apk" "$out/CitraVR-steam-frame.apk"
cp steam-frame/install.sh "$out/install.sh"
cp steam-frame/install-windows.cmd "$out/install-windows.cmd"
cp steam-frame/install.sh steam-frame/frame-register.sh steam-frame/install-library-art.py \
  steam-frame/steam-library.py "$out/files/"
cp -r steam-frame/library-art "$out/files/"
tar -czf "$out/citravr-steam-frame-files.tar.gz" -C "$out/files" .
rm -rf "$out/files"
installers=(CitraVR-Frame-Installer-Windows.exe CitraVR-Frame-Installer-macOS.zip
  CitraVR-Frame-Installer-Linux-x86_64.tar.gz CitraVR-Frame-Installer-Linux-arm64.tar.gz)
tools/make-installer.sh "$version"
for f in "${installers[@]}"; do cp "build/installer/$f" "$out/$f"; done
(cd "$out" && shasum -a 256 CitraVR-steam-frame.apk citravr-steam-frame-files.tar.gz install.sh \
  install-windows.cmd "${installers[@]}" >SHA256SUMS)

cat >"$out/notes.md" <<EOF
CitraVR - Steam Frame Edition $version.

**Install or update on a Steam Frame** (Developer Mode on, with a password): download the
installer app for your computer below and open it. It finds the Frame on the network, checks it's
ready, and installs or updates CitraVR.

| Computer | Download |
| --- | --- |
| Windows 10/11 | **CitraVR-Frame-Installer-Windows.exe** |
| macOS 12 or later | **CitraVR-Frame-Installer-macOS.zip** |
| Linux (PC, Steam Deck) | **CitraVR-Frame-Installer-Linux-x86_64.tar.gz** |
| Linux (ARM64) | **CitraVR-Frame-Installer-Linux-arm64.tar.gz** |

Or from a terminal on any computer:

\`\`\`
ssh steamos@frame.local "curl -fsSL https://github.com/$REPO/releases/latest/download/install.sh | bash"
\`\`\`

It appears in the Steam library as **Citra VR - Steam Frame Edition**. See the README for details.

The APK is signed with the project's release key, SHA-256 certificate fingerprint
\`$(echo $CERT_SHA256 | sed 's/../&:/g; s/:$//' | tr a-f A-F)\`.
EOF

echo "Release files in $out:"
ls -la "$out"

if $publish; then
  git push origin "$version"
  gh release create "$version" --repo "$REPO" --title "CitraVR - Steam Frame Edition $version" \
    --notes-file "$out/notes.md" "$out/CitraVR-steam-frame.apk" \
    "$out/citravr-steam-frame-files.tar.gz" "$out/install.sh" "$out/install-windows.cmd" \
    "${installers[@]/#/$out/}" "$out/SHA256SUMS"
fi

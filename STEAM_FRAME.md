# CitraVR on Steam Frame

Steam Frame runs Android apps through **Lepton**, Valve's Android container, with **SteamVR** as the OpenXR runtime inside it. Stock CitraVR does not start there: it requires a set of Meta OpenXR extensions that SteamVR does not offer. The `steam-frame` branch makes those features optional and adds replacements for the ones CitraVR cannot do without.

**Status: experimental.** Tested on a Steam Frame (SteamVR/OpenXR 2.18.2, Lepton Development, Android 11) on 2026-10-10. The APK installs, first-run setup works, and a homebrew title boots into a running VR session with the Frame controller bindings accepted. Not yet checked inside the headset: how the screens look (orientation, colours, sharpness), controller feel, and passthrough. See [Testing checklist](#testing-checklist).

## What changed

| Quest feature | On Steam Frame |
|---|---|
| 10 required OpenXR extensions | Only `XR_KHR_opengl_es_enable` is required. Every other extension is enabled only if the runtime offers it, and the features that need it are skipped or replaced. |
| `XR_KHR_android_surface_swapchain` (game screen, settings ribbon, keyboard and error panels; confirmed missing on SteamVR 2.18.2) | Android draws into a `SurfaceTexture`, and each new frame is copied into an ordinary GLES swapchain (`vr/SurfaceSwapchain.cpp`). Quest keeps the surface-backed swapchains. |
| `XR_FB_composition_layer_image_layout` (vertical flip) | Not needed: the copy is already upright. |
| `XR_FB_passthrough` | If passthrough is selected and the runtime offers `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND`, frames are alpha-blended over the real world with a transparent projection layer at the back. SteamVR only shows passthrough behind a projection layer. Otherwise the void is used. |
| `XR_KHR_composition_layer_cylinder` (curved screen, immersive modes) | Falls back to the flat screen. |
| `XR_EXT_performance_settings`, `XR_KHR_android_thread_settings`, `XR_META_performance_metrics` | Skipped. The ribbon's performance stats are empty. |
| Touch controller bindings | Kept. With `XR_VALVE_frame_controller_interaction`, Steam Frame controllers also get their own bindings (below). |
| HMD detection | Lepton reports model `Lepton`. It gets the Quest 3 default resolution factor (3x). |

`XR_KHR_android_create_instance` is now passed when offered, since SteamVR expects it.

## Controls

On Steam Frame controllers (if SteamVR accepts the Frame controller profile), by default:

| Steam Frame | 3DS |
|---|---|
| Right A, B, X, Y | B, A, Y, X (the Nintendo layout) |
| Left d-pad | D-pad |
| Bumpers | L, R |
| Left, right stick click | Select, Start |
| Grips | (unmapped) |
| Left stick | Circle pad |
| Right stick | C-stick |
| Left View button | Home |
| Right Menu button | Show/hide the menu bar |
| Triggers | Pointer click (on a panel); ZL, ZR elsewhere |

In VR, a panel 90° to the left shows the controllers labelled with the current mapping, and one 90° to the right the 3DS-style button layout (VR setting **Show controller diagram**, on by default, for both).

The buttons can be remapped in **Settings → VR → Controller mapping** (also in game, from the menu bar's **Settings** tab, globally or per game): tap a 3DS button, then press the controller button to use for it. Each 3DS button gets one controller button, A/B/X/Y, the d-pad, bumpers, grips, thumbstick clicks or triggers. The View and Menu buttons are mappable too, and showing/hiding the menu bar has its own row (on Menu by default; with no button for it, the menu bar stays shown). A trigger still clicks when its controller points at a panel (and that controller becomes the pointer); it presses its 3DS button only when pulled while pointing elsewhere. The sticks keep their roles.

If the Frame profile is rejected, SteamVR maps the Frame controllers onto the Quest Touch bindings. In that case, Frame's three upper face buttons on each side act as Touch B/Y.

## Switching games without leaving VR

On Steam Frame, **Close Game** (in the menu bar under the game screens) doesn't end the VR session. It pauses the game and shows a **Choose a game** panel listing your games, so you stay in VR:

- **Pick a game** to start it. A game can't be started in a VR process that already ran one, so CitraVR ends the VR process and starts the new game in a fresh one. Expect about a second of the Steam interface in between.
- **Back to game** closes the panel and resumes the current game.
- **CitraVR window** ends the VR session and returns to CitraVR's 2D window, which is what Close Game did before.
- **Quit** closes CitraVR altogether: the VR session, the 2D window and the ROM upload server (whose setting is kept, so it starts again with the app).

The panel shows the game list as last scanned by CitraVR's 2D window. ROMs added while a game is running appear after the 2D window refreshes its list. On Quest, Close Game still returns to the 2D window, which the system brings back on its own.

## Building

The APK is the same one used on Quest (`arm64-v8a`):

```sh
git submodule update --init --recursive
cd src/android
./gradlew assembleRelease
# -> app/build/outputs/apk/release/app-release.apk
```

You need JDK 17, Android SDK platform 35, and NDK `27.3.13750724`. The GitHub Actions workflow (`.github/workflows/android.yml`) builds the same APK on every push and uploads it as an artifact.

## Installing

Turn on Developer Mode on the Frame first (Steam Settings → Developer).

### Into the Steam library (recommended)

`register-frame.sh` installs CitraVR as a library title, **Library → Non-Steam → Citra VR - Steam Frame Edition**, with its own artwork, launched through Lepton like any other game. It does what Valve's SteamOS Devkit Client **Title Upload** does with runtime **Android**. It needs:
- **Developer Mode** on the Frame (Steam Settings → System), with a user password.
- **ssh access** as `steamos`. Copy your key once with `ssh-copy-id steamos@frame.local`.
- **Steam running** on the headset (it always is while the headset is on).

From the repository root, after building:

```sh
./register-frame.sh                         # installs src/android/app/build/outputs/apk/release/app-release.apk
./register-frame.sh path/to/CitraVR.apk     # or another APK
FRAME_HOST=192.168.1.42 ./register-frame.sh # if frame.local does not resolve
```

It prints one line per step:
1. **Register:** copies the APK to `~/devkit-game/citravr/` on the headset and registers it with Steam as a devkit title on the Lepton runtime. It also adds the `lepton-show-flatscreen` file, so the 2D game list is shown.
2. **Artwork:** installs the images in `steam-frame/library-art/` into Steam's artwork folder.
3. **Name:** renames the entry from "Devkit Game: citravr" to **Citra VR - Steam Frame Edition** and applies the artwork through the Steam client, without restarting it.

```text
registered as Devkit Game: citravr (Android runtime, Lepton)
Devkit Game: citravr: artwork installed for appid … in …/config/grid
steam-library: app id … is now 'Citra VR - Steam Frame Edition', 4 artwork images set
```

Run it again after each build to update. Your data is kept. `./register-frame.sh --uninstall` removes the entry, its artwork and the files on the headset.

The library title gets its **own Android data**, separate from Lepton Development's. On its first start, CitraVR asks one question, **Do you want to manually configure Citra directories?**. Answering **No** creates `/storage/emulated/0/CitraVR` (data) and `/storage/emulated/0/CitraVR/games` (games); add ROMs with the ROM upload server below.

**Folders and updates:** with all-files access, which Lepton grants, CitraVR uses its folders as plain file-system paths instead of folder grants from Android's picker. Lepton drops picker grants on every APK update, which used to leave CitraVR unable to read its folders. Folders picked earlier are switched to plain paths automatically at startup when they're on the headset's storage.

The artwork is generated from `assets/citravr_logo.png` by `steam-frame/make_library_art.py` (needs Pillow and NumPy). Rerun it after changing the design. The scripts in `steam-frame/` are shared with the Time Crisis VR Steam Frame port.

### With adb (for development)

1. On the Frame, launch **Lepton Development** from the Steam library. A floating Android home screen appears.
2. From your PC, run:
   ```sh
   adb connect frame            # or the headset's IP address
   adb install -r app-release.apk
   adb push "My Game.3ds" /sdcard/Download/
   ```
3. Open CitraVR from the Android home screen in the floating window, add `/sdcard/Download` (or wherever you pushed games) as the games folder, and start a game. The game opens in VR.

### Adding ROMs from a computer (ROM upload server)

CitraVR (any headset) can serve a web page for managing ROMs in the games folder:

1. In CitraVR's 2D window, open **ROM server** in the left menu and turn on **Enable ROM upload server**.
2. The screen shows the **Server address**, a URL such as `http://192.168.1.42:8338`. Open it in a browser on a computer on the same network.
3. The page lists the ROMs in the games folder (and subfolders up to 3 levels deep), with their sizes and the free space. Delete ROMs from there, or drop new ones onto the page. Uploads go to the top of the games folder, and the game list refreshes when each one finishes.

Notes:
- There is no password: anyone on the same network can list, upload and delete ROMs while the server is on. Use it on a trusted network only, and turn it off when you are done.
- The server keeps running in the background, with an ongoing notification, until you turn it off. It restarts with the app if left on.
- Only playable ROM types are accepted (`.3ds`, `.cci`, `.cxi`, `.3dsx`, `.app`, `.axf`, `.elf`, and their `z` compressed variants). Large files are streamed straight into the folder with no size limit beyond free space.
- **Steam Frame:** works from the network. Lepton's container shares the headset's IP address and forwards the ports apps listen on, as long as the headset isn't already using them. That's why the server uses port 8338: the Steam client already holds 8080.

Note: updating the APK keeps your folders, games and saves (see *Folders and updates* above).

## Debugging

- `adb logcat | grep -i -E "citra|openxr|steamvr"` while connected to Lepton. Startup logs which optional extensions are missing, the runtime name and version, and whether each panel uses a surface-backed or SurfaceTexture-fed swapchain.
- On the Frame itself (SSH or Konsole): `lepton dev logcat`, and the launch logs in `~/.local/share/Steam/logs/lepton-*`.
- `adb shell setprop debug.citra.dump_swapchains 1` makes CitraVR write what it copies into each VR panel to `/data/data/org.citra.citra_emu/files/swapchain_<w>x<h>.rgba` (raw RGBA, bottom row first, refreshed once a second). Pull them with `adb pull` to see exactly what SteamVR is given. Set it back to 0 when done.
- Valve's docs: [Connecting adb to Lepton](https://partner.steamgames.com/doc/steamhardware/steamframe/adb_lepton), [Steam Frame Debugging](https://partner.steamgames.com/doc/steamhardware/steamframe/debugging).

## Testing checklist

Report each of these, with the logcat for any that fail:

1. The 2D game list appears, and starting a game enters VR without a crash.
2. The game screen is upright, sharp, and not mirrored. Both eyes show their own 3D image.
3. The colours look the same as on a phone, not washed out or too dark. (This checks the sRGB handling of the copy.)
4. The lower touch screen, settings ribbon, keyboard, and error panel appear, are upright, and respond to the pointer.
5. Controller bindings: logcat shows `Using Steam Frame controller bindings`, and the table above matches.
6. Passthrough (Settings → VR → environment): the real room is visible behind the screens.
7. Performance: full-speed games stay at full speed. The extra copy costs a few hundred microseconds of GPU time per frame.

## Known limitations

- Curved screen and immersive modes fall back to the flat screen (no cylinder layers in SteamVR).
- No CPU/GPU clock control and no performance stats overlay.
- SteamVR allows 16 swapchains per session. CitraVR uses 6 (7 with passthrough).

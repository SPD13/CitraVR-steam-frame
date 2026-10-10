<h1 align="center">
  <br>
  <a href="https://github.com/amwatson/CitraVR"><img src="assets/citravr_logo.png" alt="CitraVR" width="500"></a> (Beta)
</h1>

<h4 align="center"> Play 3DS homebrew and personal game backups in 3D on the go with your Quest.
</br>
  CitraVR is a GPL-licensed, engineless OpenXR application with all source code publicly available.
</h4>

<p align="center">
  <a href="#steam-frame-edition">Steam Frame Edition</a> |
  <a href="#compatabillty">Compatibillty</a> |
  <a href="#releases">Releases</a> |
  <a href="#known-issues">Known Issues</a> |
  <a href="#how-to-install-and-run">How to Install and Run</a> |
  <a href="#building">Building</a> |
  <a href="#discord">Discord</a> |
  <a href="#need-help">Need Help?</a> |
  <a href="#support">Support</a> |
  <a href="#license">License</a>
</p>

> **This is a fork of [amwatson/CitraVR](https://github.com/amwatson/CitraVR), focused on making CitraVR run on the Valve Steam Frame.** It keeps Quest support and adds a Steam Frame edition, installed into the Frame's Steam library as **Citra VR - Steam Frame Edition**. See the **[Steam Frame guide](STEAM_FRAME.md)** to build and install it. Quest releases come from the original repository.

## Steam Frame Edition

**The primary goal of this fork is Steam Frame compatibility.** The Frame runs Android apps through **Lepton**, Valve's Android container on SteamOS, with **SteamVR** as the OpenXR runtime. Stock CitraVR doesn't start there, because it depends on Meta-only OpenXR extensions that SteamVR doesn't offer. This fork makes those optional, replaces the ones CitraVR can't do without, and adds what's needed to use CitraVR comfortably on the Frame.

**Status: experimental, running on a Steam Frame.** It has been tested with SteamVR/OpenXR 2.18.2 and Lepton (Android 11):
- the app installs into the Steam library;
- games boot in VR, with the Frame controllers;
- the in-game menu and the in-VR game list work;
- the ROM upload server is reachable from the network.

Passthrough and the immersive modes haven't been checked on the headset yet. The same APK still targets Quest.

**Quick start (Steam Frame):** build the APK (`cd src/android && ./gradlew assembleRelease`), then run `./register-frame.sh` from the repository root. It installs the app into the headset's Steam library with its artwork. Details are in [STEAM_FRAME.md](STEAM_FRAME.md).

### What this fork adds

**Running on Steam Frame (SteamVR through Lepton)**
- **OpenXR extensions:** only `XR_KHR_opengl_es_enable` is required. Every Meta/FB extension is used when the runtime offers it and skipped or replaced when it doesn't. That covers cylinder layers, image-layout flip, layer settings, passthrough, performance settings and metrics, and thread settings. `XR_KHR_android_create_instance` is passed when offered.
- **Screens without surface-backed swapchains:** SteamVR doesn't offer `XR_KHR_android_surface_swapchain`. The game screens and all VR panels (menu bar, keyboard, error dialog) are instead drawn by Android into a `SurfaceTexture`, and each new frame is copied into a regular OpenXR swapchain (`vr/SurfaceSwapchain.cpp`). New frames are detected with a frame listener. Quest keeps its surface-backed swapchains.
- **Passthrough:** without `XR_FB_passthrough`, the app uses the alpha-blend environment mode, with a transparent projection layer behind the panels.
- **Steam Frame controllers:** native bindings through `XR_VALVE_frame_controller_interaction`. A/B/X/Y are on the right controller, the 3DS d-pad on the left d-pad, L/R on the bumpers (and grips), and the left **View** button toggles the menu. Action queries also follow the OpenXR spec strictly, which SteamVR requires and Quest didn't.
- **Headset detection:** Lepton (model `Lepton`) is recognised as Steam Frame, with the Quest 3 default resolution (3×).
- **Menu bar fix:** upstream's secondary-display feature drew the lower game screen over CitraVR's own VR panel displays, which hid the menu bar and its *Close Game* button. Those displays are now excluded.

**Switching games without leaving VR (Steam Frame)**
- **The list:** on the Frame, *Close Game* no longer drops you into the Steam interface. It pauses the game and shows a **Choose a game** panel in VR, with your games and their icons.
- **Picking a game** restarts the VR part of the app on it, in about a second.
- **Back to game** resumes the current game; **CitraVR window** returns to the 2D app.

**ROM upload server (all headsets)**
- **The switch** is in *Settings → VR → ROM upload server*. The server runs in the background (foreground service) until turned off, and restarts with the app if left on.
- **The page** at `http://<headset IP>:8338` lists the ROMs in the games folder with their sizes and the free space. You can delete ROMs and upload new ones by drag and drop, with progress shown.
- **Uploads** stream straight into the games folder, so any size works. The game list refreshes by itself. The settings show the address to open.

**Steam library integration (Steam Frame)**
- **`register-frame.sh`** registers the APK with the Frame's Steam client as a title on the Lepton runtime, as Valve's devkit Title Upload does. It names it **Citra VR - Steam Frame Edition**, shows the 2D game list window, and installs library artwork. `--uninstall` removes it.
- **Library artwork:** capsule, wide capsule, hero banner, logo and icon, generated from the CitraVR logo by `steam-frame/make_library_art.py`.

**Development**
- **CI builds:** a GitHub Actions workflow builds the APK on every push.
- **Panel dumps:** `adb shell setprop debug.citra.dump_swapchains 1` saves what the app sends to the runtime for each VR panel, which helps debug visuals without wearing the headset.
- **[STEAM_FRAME.md](STEAM_FRAME.md):** technical notes, install guide, testing checklist and known limitations.

**Known limitations on Steam Frame:**
- The curved screen and immersive modes fall back to the flat screen (no cylinder layers in SteamVR).
- There's no CPU/GPU clock control and no performance overlay.
- The library title keeps its own Android data, separate from Lepton Development.

## Introduction
I originally created this project to a be a proof-of-concept of some techniques XR developers were curious about. 
Specifically:
- Building a 2D/3D hybrid app
- Creating a VR app to render 2D interactive windows without an engine
- Using VR layers to get sharp, crisp text and visuals.

CitraVR began as a Quest-native (i.e. OpenXR, without a third-party game engine) port of the [Citra 3DS emulator](https://github.com/citra-emu/citra). It seemed like a great and fun way to demonstrate all these things at once.

The original Citra project is now defunct. [Azahar](https://github.com/azahar-emu/azahar) was built on Citra's foundation and continues its development. CitraVR has since incorporated the changes from Azahar and now uses Azahar as its upstream emulator project.

The project is still small, but I'm looking for ways to improve it as time goes on.

## Features
- Stereoscopic rendering
- Broad controller support
- Large, moveable/resizeable screen
- Playable in mixed reality
- Low-overhead port
- Fully GPL-licensed, 100% independent of the Meta SDK

## Compatibillty

### HMDs
CitraVR supports the following devices:
- Meta Quest 2
- Meta Quest Pro
- Meta Quest 3
- Meta Quest 3S
- Steam Frame (experimental, see [STEAM_FRAME.md](STEAM_FRAME.md))

### Games
For a full list of games that work well on CitraVR, please visit the [CitraVR Game Compatability List](https://docs.google.com/spreadsheets/d/1viN8_MWO1HW9QXlkT-TdCGQbH1g660mKsIp1ZTARdho/edit?usp=sharing)

### Controllers/Input 
[Touch Controller Input Bindings Diagram](https://github.com/amwatson/CitraVR/wiki/Touch-Controller-Input-Bindings)

For games that need access to more inputs, or if a player needs to access more inputs faster, CitraVR also supports a multitude of 3rd party wired USB and wireless bluetooth controllers. 

## Releases
Grab the latest release [here](https://github.com/amwatson/CitraVR/releases)

## Known Issues
See the [CitraVR Known Issues](https://github.com/amwatson/CitraVR/wiki/CitraVR-Known-Issues)

## How to Install and Run
- [How to install and run CitraVR on Quest](https://github.com/amwatson/CitraVR/wiki/Install-Run-on-Quest)
- [How to back up 3DS Games](https://github.com/amwatson/CitraVR/wiki/Backing-up-3DS-Games)

## Building
[Building for Quest](https://github.com/amwatson/CitraVR/wiki/Building-for-Quest)

## Discord 
Join the [Flat2VR](https://flat2vr.com/) discord and from there join [cvr-join](https://discord.com/channels/747967102895390741/1196505250102792232) to get access to the CitraVR community and support forums

# Need Help?
Please check our [Troubleshooting](https://github.com/amwatson/CitraVR/wiki/Troubleshooting) and [Known Issues](https://github.com/amwatson/CitraVR/wiki/CitraVR-Known-Issues) pages to see if your issue is listed.
To file a bug report or a feature request, please [submit an issue](https://github.com/amwatson/CitraVR/issues/new/choose).
Otherwise, follow the instructions for <a href="#discord">Discord</a> and post in [cvr-support](https://discord.com/channels/747967102895390741/1196505719910957176)

## Support
[Buy me a beer](https://www.buymeacoffee.com/fewerwrong)

## License
CitraVR is licensed under the GPLv3 (or any later version). Refer to the [LICENSE.txt](https://github.com/amwatson/CitraVR/blob/master/license.txt) file.

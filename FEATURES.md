# What this fork adds

Everything the Steam Frame Edition changes or adds to [CitraVR](https://github.com/amwatson/CitraVR). The [README](README.md) has the summary, the controls and the install guide.

## Running on Steam Frame (SteamVR through Lepton)
- **OpenXR extensions:** only `XR_KHR_opengl_es_enable` is required. Every Meta/FB extension is used when the runtime offers it and skipped or replaced when it doesn't. That covers cylinder layers, image-layout flip, layer settings, passthrough, performance settings and metrics, and thread settings. `XR_KHR_android_create_instance` is passed when offered.
- **Screens without surface-backed swapchains:** SteamVR doesn't offer `XR_KHR_android_surface_swapchain`. The game screens and all VR panels (menu bar, keyboard, error dialog) are instead drawn by Android into a `SurfaceTexture`, and each new frame is copied into a regular OpenXR swapchain (`vr/SurfaceSwapchain.cpp`). New frames are detected with a frame listener. Quest keeps its surface-backed swapchains.
- **Passthrough:** without `XR_FB_passthrough`, the app uses the alpha-blend environment mode, with a transparent projection layer behind the panels. On Steam Frame, passthrough is the default background, as on Quest 3. A **Passthrough: On/Off** button in the menu bar (Main tab) switches between the room and the void in game, and keeps the choice as the VR background setting. While the room shows, the controller diagram and 3DS layout panels are hidden.
- **Steam Frame controllers:** native bindings through `XR_VALVE_frame_controller_interaction`. The face buttons are on the right controller (in the Nintendo layout by default), the 3DS d-pad on the left d-pad, L/R on the bumpers, and the right **Menu** button toggles the menu bar; the buttons can be remapped (see [Controller mapping](#controller-mapping-vr-settings)). Action queries also follow the OpenXR spec strictly, which SteamVR requires and Quest didn't.
- **Headset detection:** Lepton (model `Lepton`) is recognised as Steam Frame, with the Quest 3 default resolution (3×).
- **Menu bar fix:** upstream's secondary-display feature drew the lower game screen over CitraVR's own VR panel displays, which hid the menu bar and its *Close Game* button. Those displays are now excluded.
- **Clean lower screen:** SteamVR depth-tests composition layers instead of drawing them in order as Quest does. The lower screen shared its plane with the menu bar's translucent background, and the two z-fought into a moving, CRT-like pattern of black pixels. The lower screen is now drawn just in front of the menu bar, and the pointer dot just in front of whatever it points at.
- **Sharp panels:** the top screens and the lower screen each get their own swapchain. The lower screen gets a filtered downscale to twice the 3DS resolution, since it's shown smaller than its texels. Every swapchain stays at most 1000 px tall, below the size Valve's injected foveated-rendering layer treats as an eye buffer.

## Controllers and pointer ray
- **Controllers:** your controllers are drawn in VR, using SteamVR's own render models of the Steam Frame controllers, read from the headset's SteamVR install and placed with the models' `openxr_aim` reference, as SteamVR places them.
- **Pointer ray:** a ray runs from the pointing controller to the cursor, so you can see where you point. When it points at nothing, a short ray fades out.
- **Reliable clicks:** pulling the trigger on a button clicks it right away, where you aimed just before the pull (the pull tilts the controller); what the hand does until the release doesn't matter. The menus don't drag: lists scroll with the thumbstick. Select and Start in the menu bar stay pressed while the trigger is held.
- **Settings:** **Show controllers** and **Show pointer ray** in the VR settings turn each on or off (both on by default; they apply from the next game start).
- **How:** SteamVR on Steam Frame draws quad layers over projection layers whatever their order, so both are quad layers, which it sorts by depth among the panels. The ray is a thin quad along the ray, turned towards the head. Each controller is drawn, per eye, into a quad at the controller that faces that eye and is shown to it only, rendered from that eye's position so the stereo pair is exact. Where no model is found (e.g. on Quest), only the ray is drawn.

## Controller mapping (VR settings)
- **The screen:** **Settings → VR → Controller mapping**, also in game from the menu bar's **Settings** tab. It lists the 3DS buttons (**[3DS] A**, B, X, Y, L, R, ZL, ZR, Start, Select, Home and the d-pad), each with the controller button that presses it. In VR, tapping a 3DS button asks you to press the controller button to use for it (or **None** to unmap it); the game doesn't receive that press. The 2D settings show a list to pick from instead.
- **One to one:** a 3DS button has one controller button, and a controller button presses one 3DS button: picking a button that was used elsewhere clears it there.
- **Mappable:** A, B, X, Y, the d-pad, both bumpers, both grips, both thumbstick clicks, both triggers, the left **View** button and the right **Menu** button. The thumbsticks (Circle Pad and C-Stick) keep their roles.
- **Menu bar:** showing/hiding the menu bar is a row of its own (**[VR] Show/hide menu bar**), on the Menu button by default, so it can move to another button. With no button for it, the menu bar stays shown, since it holds the settings.
- **Triggers:** a trigger pulled while its controller points at a panel (the bottom screen, the menu bar, the keyboard…) clicks, and that controller becomes the pointer. Pulled while pointing anywhere else, it presses its mapped 3DS button (ZL/ZR by default). The role is set when the trigger is pulled and kept until it's released. To map a trigger, point away from the menu (the game screens are fine) and pull it when asked.
- **Defaults (Steam Frame):** the Nintendo layout on the face buttons (the 3DS's B where the Frame's A is: A→B, B→A, X→Y, Y→X), the d-pad on the d-pad, L/R on the bumpers, Select/Start on the left/right stick clicks, ZL/ZR on the triggers, Home on the View button and the menu bar on the Menu button; the grips are free. Headsets without bumpers or a right Menu button (Quest) have L/R on the grips and the menu bar on the left Menu button.
- **Per game:** with **Per-game** selected in the in-game settings, the mapping is saved for the running game only. Changes apply right away, also mid-game.
- **Resets and copies:** **Reset to default** puts every button back on its default (globally, or for the game). With **Per-game** selected, **Apply to global** makes the game's mapping the global one, and **Use the global mapping** removes the game's own mapping.

## Controller diagram in VR
- **Where:** a panel 90° to the left of the game shows the Steam Frame controllers: turn your head to check which button does what, without leaving the game.
- **Live labels:** the labels come from the mapping in use, per-game mapping included, and change as soon as you remap a button, also from the in-game settings.
- **3DS layout:** a panel 90° to the right shows the 3DS-style button layout, to compare.
- **Setting:** **Show controller diagram** in the VR settings (on by default) hides both panels, right away. They're also hidden while passthrough shows the room.
- **Controls tab:** the menu bar's **Controls** tab is a help page with the same two drawings, one at a time (**VR controllers** / **3DS buttons**), labelled with the mapping in use. It works whatever the setting and with passthrough on.

## One-question setup and folders that survive updates
- **First launch:** when CitraVR has all-files access (always on Steam Frame), first launch asks a single question: *Do you want to manually configure Citra directories?* Answering **No** creates `/storage/emulated/0/CitraVR` (data) and `/storage/emulated/0/CitraVR/games` (games) and goes straight to the game list.
- **Plain paths:** CitraVR's folders are used as plain file-system paths rather than grants from Android's folder picker. Lepton drops picker grants on every app update; plain paths aren't affected. Folders picked earlier are switched to plain paths automatically.
- **Exact file names:** files created in plain-path folders keep their exact names (Android's document API would add `.bin`, which made per-game settings unreadable). Config files left with a `.bin` suffix by earlier builds are renamed at startup.

## Switching games without leaving VR (Steam Frame)
- **The list:** on the Frame, *Close Game* no longer drops you into the Steam interface. It pauses the game and shows a **Choose a game** panel in VR, with your games and their icons.
- **Picking a game** restarts the VR part of the app on it, in about a second.
- **Back to game** resumes the current game; **CitraVR window** returns to the 2D app; **Quit** closes CitraVR altogether (the ROM upload server too, until the next start).

## ROM upload server (all headsets)
- **The screen:** a **ROM server** item in CitraVR's left menu opens it, with the on/off switch, the address to open, the server status and the games folder. The server runs in the background (foreground service) until turned off, and restarts with the app if left on.
- **The page** at `http://<headset IP>:8338` lists the ROMs in the games folder with their sizes and the free space. You can delete ROMs and upload new ones by drag and drop, with progress shown.
- **Uploads** stream straight into the games folder, so any size works. The game list refreshes by itself. The settings show the address to open.

## Steam library integration (Steam Frame)
- **One-command install:** releases on GitHub come with `install.sh`, which runs on the headset (over ssh): it downloads the release, checks it, and registers the APK with the Frame's Steam client as a title on the Lepton runtime, as Valve's devkit Title Upload does. It names it **Citra VR - Steam Frame Edition**, shows the 2D game list window, and installs library artwork. The same command updates it; `--uninstall` removes it.
- **Windows installer:** `install-windows.cmd`, double-clicked on a Windows 10/11 PC, asks whether to install, update or uninstall and for the headset's address, checks the headset answers, and runs `install.sh` on it through Windows' ssh client (offering to add the client when it's missing). It runs `steam-frame/install-windows.ps1`, which can also be pasted into PowerShell.
- **`register-frame.sh`** does the same with an APK built from source, for development.
- **Releases:** `tools/make-release.sh` builds, signs (with the project's release key), packages and publishes them.
- **Library artwork:** capsule, wide capsule, hero banner, logo and icon, generated from the CitraVR logo by `steam-frame/make_library_art.py`.

## Development
- **Local builds:** there's no CI; APKs and releases are built locally (see the [Steam Frame guide](STEAM_FRAME.md) and `tools/make-release.sh`).
- **Panel dumps:** `adb shell setprop debug.citra.dump_swapchains 1` saves what the app sends to the runtime for each VR panel, which helps debug visuals without wearing the headset. `2` saves the next 30 frames instead, and `debug.citra.dump_width` limits dumps to the panels of one width.
- **Lower screen resolution:** `adb shell setprop debug.citra.lower_panel_resolution <factor>` changes the lower screen's resolution while a game runs (multiples of 320×240, default 2, up to 3).
- **[STEAM_FRAME.md](STEAM_FRAME.md):** technical notes, install guide, testing checklist and known limitations.

Known limitations are listed in the [README](README.md#steam-frame-edition) and in [STEAM_FRAME.md](STEAM_FRAME.md).

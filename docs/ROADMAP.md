# Roadmap

What is planned, in rough order. ✅ done · 🔧 in progress · 📋 planned. No dates: the order is firm, the timing is not.
Non-original features are opt-in where they change gameplay; display and input improvements may default on.
Where each feature of the PS3 game stands today, and the port-only features with their defaults: [FEATURES.md](FEATURES.md).

[Release criteria](RELEASE-CRITERIA.md) say when a tag is a preview and when the port counts as playable.

## 1. The campaign, playable at speed  🔧

| | |
|---|---|
| ✅ | Boots, menus, movies, first-boot flow, saves the profile |
| ✅ | First level runs and renders its real scene (deferred lighting, shadows, depth of field, effects) |
| 🔧 | **Performance:** levels run at about 16-22 frames/s today (about 22-23 in a busy Frozen Shores window); the goal is a steady 30 fps (the renderer's command walker and the SPU job code are the limits). With the shipped pipeline list (on by default) a first visit's busy minute runs at about 16.5 frames/s with little stutter |
| ✅ | **In-level HUD** (ammo counter, crosshair, objectives): drawn in level 2; in the first level's opening the game itself shows only the objective marker and reticle |
| ✅ | **In-level play by hand:** a hand-played session with a real controller walked, aimed, fired, crouched, reloaded and threw grenades; pause, quit to the menu and Continue work |
| 🔧 | **Audio:** the engine fills its output in the level; it stutters now and then |
| ✅ | **Update 1.14 as the base version** (it adds the option to skip the mandatory motion-control sections): the default build; all nine chapters load on it in scripted runs. 1.00 can still be built (`KZ3_EBOOT_VER=100`) |
| 🔧 | The whole campaign, start to finish: all 9 chapters load and play for minutes; one chapter (Pyrrhus Outskirts) has been played by hand to its end (2026-10-20), the others not played through. Remaining: level streaming, scripts, physics, AI, every mission |
| 🔧 | **Error and runtime logging:** per-run log files, crash reports with an error box, quiet by default |
| 🔧 | Save games in your user folder (checkpoints save and reload today); local trophies (shown by the port, not synced anywhere) are planned |
| ✅ | **Installer and portable zip** (Windows): check your own disc dump and 1.14 update, then build the port on your machine with a bundled compiler; nothing of the game is shipped or downloaded. Published as pre-releases (latest <!-- version -->v0.1.3<!-- /version -->) |

## 2. PC display and settings  🔧

- ✅ Windowed and borderless windows, Alt+Enter, exclusive fullscreen as a setting, picture stretched or kept at its own shape, bilinear or nearest filtering, VSync on/off (Options > Display); checked by hand at 1920x1080
- ✅ Widescreen: in gameplay the view follows the window's shape (wider on ultrawide, taller on 16:10); the HUD, menus, subtitles and movies stay 16:9 and centred; world markers follow the same view
- 📋 Internal resolution 720p up to 4K, with anti-aliasing at the higher resolution
- ✅ Frame rate limit of 30, 60 or 120 (Options > Display)
- 📋 Variable refresh rate, low-latency mode
- 📋 60 fps and above (opt-in where it would change gameplay)
- 📋 Motion blur strength setting (needs a clean way to switch the game's blur pass)
- 📋 Temporal anti-aliasing and upscaling (TAA, DLAA, DLSS, FSR), after the Windows build matches the console
- 📋 Ray-traced sun shadows, then ambient occlusion and reflections (off by default)
- 🔧 A settings menu in the game's own UI style (the Display and Mouse pages exist; more pages to come)

## 3. Controls  🔧

- Gamepads: Xbox (XInput) works today, and DualShock/DualSense through Steam Input; native PlayStation, Switch Pro and Xbox pads without Steam (SDL3, with rumble and motion sensors) are built in (not yet confirmed on real hardware)
- **Keyboard and mouse** with native mouse aim (no emulated stick), aim assist off by default on mouse (mouse look on foot is on by default, `KZ3_MOUSE_LOOK=0` turns it off; sensitivity under Options > Mouse)
- **Rebindable controls:** every action, primary and secondary key, conflict warnings, reset to defaults
- ✅ **Button prompts** that follow the device in use: PlayStation, Xbox or keyboard/mouse glyphs, drawn to match the
  game's own (Options > Controls > Advanced Settings > Layout and Prompts)
- Motion-control sections playable without motion sensors (stick or mouse)
- 📋 Aim response (lighter stick smoothing) as an opt-in setting
- 📋 Minimap with enemy markers (opt-in; changes gameplay)

## 4. Multiplayer and Botzone  🔧

- ✅ **Local split-screen co-op** campaign: works on one PC with two controllers (first played 2026-10-22); the two views sit slightly out of place and the right view can flicker
- 🔧 **Botzone** (offline matches against bots): Botzone reaches Career Selection and campaign levels 200-208 all load (2026-10-27, scripted runs); no full match played by hand; hand sessions on 2026-10-22 under Proton stopped at the syncing screen
- **Online multiplayer** in place of PSN, through a self-hosted server: the game's own network code against a Medius-compatible server, with [Nakama](https://heroiclabs.com/nakama/) as the fallback:
  matchmaking, lobbies, Warzone and Guerrilla Warfare, up to 24 players
- **Online co-op campaign** (wanted): the second player joins the host's split-screen co-op from another PC, after local co-op works and the campaign is playable
- Server choice in a settings page: a default server or any community server (address, key, TLS)
- Goes online only when you choose online play, and only to the server you set
- DLC map packs from your own purchased copies
- 📋 **In-game voice chat** for multiplayer and the online co-op campaign (microphone and headset; online only when you choose online play)

## 5. Platforms  📋

- **Native Linux build** (no Proton needed): a Vulkan graphics path, Linux packaging (AppImage, Flatpak or tarball)
- **Native macOS build** (Apple silicon), after the Linux build
- **Android (ARM64)** build, late: you build the APK on your own PC from your own game files
- Windows remains a first-class target

## 6. Later  📋

- **More renderers:** once the game works end to end on Direct3D 12, review Vulkan and OpenGL paths beside it,
  with the choice offered in the installer (Direct3D 12 stays the default)
- **Mod support:** data mods as folders loaded over the game's archive (with a reader/writer for the game's data
  format), later code mods
- PlayStation Move controllers and stereoscopic 3D output: planned, off by default, after the campaign plays at speed
- 📋 Saves in the PS3's own format that you can copy between the port, RPCS3 and a console (the profile file is written in that format already; opening a port save elsewhere is untested)
- **VR** (SteamVR / OpenXR), if it can be done well
- **Going native, after feature parity and VR:** replace the run-time layers step by step: a native renderer fed by
  the engine's own draw lists, native SPU jobs, then rewritten game code where it pays off
  ([what is native today](HOW-IT-WORKS.md#what-kind-of-port-is-this))
- 🔧 **Discord Rich Presence** (built, opt-in via `discord_presence = 1` in `kz3.ini`, still being tested): show "Playing Killzone 3" with the current chapter in your Discord status,
  through the Discord desktop app on your own PC; nothing is sent when it is off

## Not planned

- Distributing the game or any part of it: you always bring your own copy

Where each feature stands: [FEATURES.md](FEATURES.md).

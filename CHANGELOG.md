# Changelog

All notable user-facing changes, newest first, in the style of [Keep a Changelog](https://keepachangelog.com/). The
project is a pre-release (partly playable): see [RELEASE-CRITERIA.md](docs/RELEASE-CRITERIA.md).

## [Unreleased]

## [0.1.6] - 2026-10-29 (pre-release)

### Added
- The setup finds a portable RPCS3 (an unzipped folder with `rpcs3.exe` beside its `config`) in Downloads, Desktop, Documents, the program-files and games folders, the drive roots and beside the installer, and accepts its `config` or `dev_hdd0` folder as the folder you pick. A moved, localised or OneDrive-redirected Desktop, Documents or Downloads folder is searched too (read from Windows' user shell folders; checked by host tests, not yet on Windows).
- The port's own screens follow the game's language through `lang\port_ui.tsv`, which the setup now installs next to `kz3.exe` (the translations are machine-made; corrections are welcome).
- Closing the window asks through the game's own exit dialog (in a level the pause menu opens first) instead of quitting at once.
- The window remembers its place and size between starts (`display.ini`). Maximizing the window switches to borderless full screen, and F11 toggles it.
- Options > Display > When unfocused (Keep running, Mute, Pause): what the game does when its window loses focus. Pausing does not apply in online play.
- Keyboard and mouse count as their own controller when a pad is connected, so local split-screen co-op can be played with one pad plus the keyboard and mouse (the keys act for player 1 until the game gives the keyboard a player).

### Fixed
- Exclusive full screen (Options > Display > Fullscreen, or `mode = fullscreen` in `display.ini`) resizes the picture after every switch, and if Windows refuses a frame in full screen the game drops to borderless full screen for the session and keeps drawing instead of freezing (the choice stays in `display.ini`; the Options row then reads Borderless). Checked under Proton, including forced failures; not yet confirmed on native Windows.
- In first person, the arms and weapon follow the view while you turn with the mouse instead of jittering against it. Checked under Proton in the jungle level: the arms stayed within about 1 pixel of the view (before: a spread of about 13 pixels, up to 66).
- The setup's log no longer keeps your Windows user name when a path is written in its escaped form (doubled backslashes, as in an error message or a list of paths) or URL-encoded. Reported by @feanor-noldorin (project-pyrrhus#9).
- `bump_version.py` keeps each file's line endings instead of writing Windows line endings when run on Windows. Reported by @feanor-noldorin (project-pyrrhus#9).
- The helmet visor keeps its authored field of view at any Field of view setting, and cinematics keep the game's own field of view (`KZ3_FOV_VISOR=0` and `KZ3_FOV_CINE=0` turn these off). Checked on the field-of-view test sheet and a start-up run.
- Audio: the audio thread is kept on the performance cores of hybrid CPUs. In our paired test runs the audio thread's missed deadlines fell from about 200 to about 41 per 10 seconds; the frame rate did not change.
- The game's clock calls use the Windows wall clock (an upstream ps3recomp fix, #633), so time reads advance as the game expects.
- On laptops with two GPUs, the game now asks Windows for the high-performance GPU instead of taking Windows' default adapter, and the log lists every GPU found and the one chosen. On the sister port, which shares this code, a player's log showed the integrated GPU chosen on a laptop that also has a dedicated GPU (reported by @feanor-noldorin, project-pyrrhus#13); not reproduced here. Checked under Proton; not yet confirmed on a hybrid laptop.
- Building the port from source on native Windows (Git Bash): the build scripts find a real Python (never the Microsoft Store shortcut), stop early with a clear message when git's line-ending conversion rewrote the sources, and the Linux-only host tests are skipped on Windows. Reported by @feanor-noldorin (project-pyrrhus#9).
- When the PC clock is behind the install's files (the build would repeat itself), the setup stops with a message that shows both times and how to set the clock.
- An update folder without `USRDIR\added_data_ps3.psarc.edat` is refused by the setup's first check as well as by its update page, with a message that names the file and says what it holds (the 1.14 menu screens, the multiplayer map scripts and files that override the disc's).
- The setup's folder hints offer only Killzone 3 dumps (other games' dumps on a drive root are not named), name every `.iso` file in the folder or one level down on a drive root beside the folder hints, and take the update's `USRDIR` as the update folder even when it holds a `PARAM.SFO` (reported by a player on Reddit). The interactive check stops with its FAIL lines when no input is left instead of repeating.

## [0.1.5] - 2026-10-28 (pre-release)

### Fixed
- Settings you add to `kz3.ini` by hand (for example `discord_presence = 1`) are kept when the game starts. Comments in the file are not kept.
- When the game opens a file to create, truncate or append to it, the request is read with the PS3's flag values. No visible change from this has been seen yet.
- When the setup stops, its message box says why (the same reason the log has), and the next start asks again for the file that was wrong. The EBOOT page refuses the disc's 1.00 EBOOT and names the update's USRDIR folder, where the 1.14 one is (#8, thanks @genericuser001).
- With a decrypted ISO as the disc dump, the first check no longer stops with "No module named 'iso9660'" (#15, thanks @MOW531).
- When a game-file check fails, the setup and the first start say which input failed (disc dump, 1.14 update or EBOOT), the path, what is there and what it must hold. They spot a swapped disc and update, or a drive root, and say which inputs passed. The README starts with a Windows quick start (#10, #12, #13, thanks @Tzeentchnet and @Jayinem).

### Added
- Options > Display > Discord status (On/Off, off by default): shows "Playing Killzone 3" and your chapter on your Discord profile through the Discord app on this PC; the choice is saved in kz3.ini (discord_presence).
- Start menu entry "Discord status on or off": shows whether the Discord status is on and switches it (the port tells the Discord app on this PC what to show on your profile; nothing is sent when it is off; the Discord app must be running).

## [0.1.4] - 2026-10-28 (pre-release)

### Fixed
- Mouse look no longer holds still and then jumps to a new direction in busy scenes (seen at the start of the first level): mouse, keyboard and pointer input is now read on every frame tick. In our tests at that spot the view follows the mouse without jumps.
- Opening Options > Wireless Controller or Options > Display after installing a new version no longer ends the level with error 20000: the game's file cache is rebuilt once when the installed version changes, so the first start after an update loads a little slower.
- The game log now records the scheduler and memory lines that tell us whether a fix is in your build.
- When the first start asks for the decrypted EBOOT, a folder is accepted too: the setup looks for EBOOT.elf in it and in its USRDIR folder, and says so when it is not there.

## [0.1.3] - 2026-10-28 (pre-release)

### Added
- A disc dump whose game archive is split into parts (data_ps3.psarc.66600, .66601, and so on, as a FAT32 drive stores it) is accepted as it is, by the setup, the first-run check and the game. A missing part is named.
- The Asian (BCAS25008) and Japanese (BCJS30066) discs are accepted as untested regions, and a 1.14 update from any of the four regions is accepted with any accepted disc (the setup shows a note when they differ; in our tests with US and EU discs it gave the same game data).
- The first run checks the disc's game archive against the known copy (A1) and says when it differs; the launcher log starts with the version line.
- A new page, docs/GETTING-FILES.md, shows what each of the three inputs must contain and what every first-run check code means.
- The game window shows the project's icon in its title bar and taskbar.

### Fixed
- The game stopped a few seconds after a level started (for example after the chapter 1 movie): the setup built the game with the wrong table of replaced functions for the 1.14 update. Install this version over the old one: it lifts and builds the game again.
- The game folders are given once: when the build is started later or a check fails, the first start checks the folders from the setup instead of asking for them again.

## [0.1.2] - 2026-10-28 (pre-release)

### Added
- A decrypted disc ISO works as the game source, next to a folder dump: the setup copies the game files it needs out of the image once (about 10 GB). An encrypted ISO is refused.
- The EU disc (BCES01007) is accepted as an untested region, with its own 1.14 update.
- The disc page reads the disc's title id and names it; picking PS3_GAME or USRDIR instead of the dump folder works too. Other games (for example Killzone Trilogy) are refused on that page, and the update must belong to the same release as the disc.
- Installing into a OneDrive folder is refused, and game files that are only online in OneDrive are named with what to do.

### Fixed
- On PCs in time zones west of UTC (the Americas) the build stopped with "manifest 'build.ninja' still dirty after 100 tries": installed files now carry the right local time. If an older install shows this, the first run now says so plainly; reinstall with this version.
- The setup log no longer lists the crash-report address.

## [0.1.1] - 2026-10-27 (pre-release)

### Added
- Opt-in crash reports: when turned on (installer, Advanced page, or "Crash reports on or off" in the Start menu), the launcher sends one scrubbed report after a crash; never a memory dump, never a crash from before you turned it on.

## [0.1.0] - 2026-10-27 (pre-release)

First published release: a Windows installer and a portable zip that check your own disc dump and 1.14 update, then
recompile and build the port on your machine.

### Added
- Windows installer and portable zip with a bundled compiler, one progress bar, a full install log, a desktop shortcut and an uninstaller.
- Opt-in check for a newer version (one anonymous request to GitHub, only when you turn it on).
- Update 1.14 as the base version, with its balance changes and Motion Sensor option (partial: not every 1.14 addition is checked yet).
- Ultrawide and widescreen: the 3D view follows the window's shape while the HUD, menus, subtitles and movies stay 16:9 and centred, with name tags on target (objective markers not yet checked).
- Field of view setting (Options > Display).
- Frame rate limit of 30, 60 or 120 (Options > Display).
- Bloom strength setting (Options > Display).
- Split-screen co-op campaign on one PC with two controllers (partial: the two views still sit slightly out of place).
- Botzone corner minimap drawn at 16:9 and 32:9 (checked in scripted runs).
- Gamepad support through SDL3, with the game's own controller options.
- Direct3D 12 renderer for NVIDIA GPUs; title screen, main menu, attract mode and in-level play (walk, aim, fire, cover, pause menu, HUD).

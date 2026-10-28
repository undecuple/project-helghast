# Changelog

All notable user-facing changes, newest first, in the style of [Keep a Changelog](https://keepachangelog.com/). The
project is a pre-release (partly playable): see [RELEASE-CRITERIA.md](docs/RELEASE-CRITERIA.md).

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

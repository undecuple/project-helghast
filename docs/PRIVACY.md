# What goes online and where your data goes

## What goes online

Nothing, unless you ask for it. Each item is off until you turn it on, and this list names every one. No crash reports are sent: when the game crashes it writes a crash file and a minidump next to its logs, and they stay on your PC.


- **New-version check** (installer and portable zip): one anonymous request to GitHub for this
  repository's latest release, made only when you click "Check now" or tick "Check for new versions" (then at most
  once a day). Nothing about you, your computer or the game is sent; a new installer downloads only when you click it.
- **Project website** (`site/`): viewing it loads the Rajdhani and Barlow fonts from Google Fonts, which sees your visit. The repository, installer and game do not.
- **Intel XeSS runtime download** (experimental, only if you choose XeSS as the upscaler): when the launcher
  finds XeSS selected and its runtime missing or not matching, it asks first and downloads only if you click Yes.
  It fetches Intel's XeSS SDK 3.0.2 archive (about 73 MiB) from Intel's official GitHub release
  (`https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip`), an ordinary anonymous download
  that sends nothing about you or the game. The launcher checks the archive's SHA-256, the size and hash of
  every file it keeps, and Intel's code signature before placing `libxess.dll` and its licence texts beside the game.
  If you click No, nothing is downloaded and the game keeps the upscaler you had before.
- **Online multiplayer** (planned): only through a self-hosted server you choose.
- **Discord status** (optional, off unless you turn it on in Options > Display > Discord status, with "Discord status on or off" in the Start menu, or with `discord_presence = 1` in `kz3.ini`): the port tells the Discord
  app on your own PC what to show on your profile (the activity text, the game's process id and the port's Discord
  application id), through the app's local connection. The port itself sends nothing over the network for it; the
  Discord app shares the status as it does for other games. Still being tested.

The original PS3 game's online play is kept running by the PS Rewired community
([their Killzone 3 page](https://psrewired.com/servers/23044)). This port is not affiliated with or endorsed by
PS Rewired and does not connect to their servers.

## Where your data goes

Saves and settings go to `%APPDATA%\Project Helghast`, and caches (shaders) to `%LOCALAPPDATA%\Project Helghast`.
To keep everything in the game folder instead, drop an empty file named `portable.txt` next to the launcher. For one
run only, start the launcher with `--portable` or `--appdata` (no file changes). Always start the game through the
launcher: `kz3.exe` started directly does not apply any of this. The launcher's log (`logs\launcher.log`) and the
Start-menu "Open saves folder" entry say which rule decided, for example `data: portable (portable.txt)`. The
"Move saves and settings" entry moves your files and creates or removes `portable.txt` to match.

# What goes online and where your data goes

## What goes online

Nothing, unless you ask for it. Each item is off until you turn it on, and this list names every one:

- **New-version check** (installer and portable zip): one anonymous request to GitHub for this
  repository's latest release, made only when you click "Check now" or tick "Check for new versions" (then at most
  once a day). Nothing about you, your computer or the game is sent; a new installer downloads only when you click it.
- **Crash reports** (installer, Advanced page, or "Crash reports on or off" in the Start menu): when turned on, the
  launcher sends one report after a crash (at most one per session): the crash reason, the stack addresses and the
  last 200 lines of the port's log (64 KB at most), with user names, machine names, IP and e-mail addresses, online
  ids and your install and game folders removed, to the project's own error tracker over HTTPS. No usage data, never
  a memory dump (the minidump stays on your PC), and crashes from before you turned it on are not sent. The tracker
  sees your IP address as part of the connection, as any web server does. Off unless you turn it on; builds without
  a tracker address show the option as not available.
- **Project website** (`site/`): viewing it loads the Rajdhani and Barlow fonts from Google Fonts, which sees your visit. The repository, installer and game do not.
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

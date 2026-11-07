# What goes online and where your data goes

## What goes online

Nothing, unless you ask for it. Each item is off until you turn it on, and this list names every one. No crash reports are sent: when the game crashes it writes a crash file and a minidump next to its logs, and they stay on your PC.


- **New-version check** (installer and portable zip): one anonymous request to GitHub for this
  repository's latest release, made only when you click "Check now" or tick "Check for new versions" (then at most
  once a day). Nothing about you, your computer or the game is sent; a new installer downloads only when you click it.
- **Project website** (`site/`): viewing it loads the Rajdhani and Barlow fonts from Google Fonts, which sees your visit. The repository, installer and game do not.
- **Experimental networking** (off by default): setting `KZ3_ONLINE` to a nonempty value other than `0` opts in.
  The port reads the selected server from `online.ini` and may resolve its host through the operating system,
  which can contact the configured DNS service. The existing guest policy is intended to restrict destinations
  to the selected server, but the generic resolver-policy gap remains unverified. An inherited `PSNR_SERVER` can
  also enable the runtime's separate psnr connection on valid startup; unset it if that connection is not intended.
  Invalid online setup stops the game before guest execution. With `KZ3_NET_CAPTURE` also
  enabled and a loopback server selected, diagnostic listeners bind `127.0.0.1` on ports 10070, 10071, 10075,
  10078 and 10079. Multiplayer and compatibility with any server are unverified:
  [network evidence and limits](NETWORK-EVIDENCE.md).
- **Discord status** (optional, off unless you turn it on in Options > Display > Discord status, with "Discord status on or off" in the Start menu, or with `discord_presence = 1` in `kz3.ini`): the port tells the Discord
  app on your own PC what to show on your profile (the activity text, the game's process id and the port's Discord
  application id), through the app's local connection. The port itself sends nothing over the network for it; the
  Discord app shares the status as it does for other games. Still being tested.

The original PS3 game's online play is kept running by the PS Rewired community
([their Killzone 3 page](https://psrewired.com/servers/23044)). This port is not affiliated with or endorsed by
PS Rewired; it has no default server and compatibility with their service has not been verified.

## Where your data goes

Saves and settings go to `%APPDATA%\Project Helghast`, and caches (shaders) to `%LOCALAPPDATA%\Project Helghast`.
To keep everything in the game folder instead, drop an empty file named `portable.txt` next to the launcher. For one
run only, start the launcher with `--portable` or `--appdata` (no file changes). Always start the game through the
launcher: `kz3.exe` started directly does not apply any of this. The launcher's log (`logs\launcher.log`) and the
Start-menu "Open saves folder" entry say which rule decided, for example `data: portable (portable.txt)`. The
"Move saves and settings" entry moves your files and creates or removes `portable.txt` to match.

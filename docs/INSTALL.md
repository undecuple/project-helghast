# Installing on Windows

Get the installer or the portable zip from the [releases page](https://github.com/undecuple/project-helghast/releases). Check it against `SHA256SUMS`. No game code, assets or keys are ever shipped or downloaded. The setup builds the game's code on your own machine from your own copy, with a bundled compiler. What a release must meet to be called playable is in [RELEASE-CRITERIA.md](RELEASE-CRITERIA.md).

## Windows quick start

1. **Have your three inputs ready.** All three come from your own copy of the game. [GETTING-FILES.md](GETTING-FILES.md) says what each must hold.
   - The disc dump: the folder that holds `PS3_GAME`, or a decrypted `.iso` file.
   - The 1.14 update folder: the one that holds `PARAM.SFO` and `USRDIR`.
   - The decrypted 1.14 EBOOT, `EBOOT.elf`.
2. **Run `Project-Helghast-setup.exe`.** It installs for your user only and needs no administrator rights.
3. **Answer its pages.** They ask for the setup type, the install folder and the tasks (desktop shortcut, "Build now"). They also ask for the disc dump (a folder, or "Browse for ISO"), the 1.14 update folder and the decrypted EBOOT. The PS3 fonts and the advanced options are optional. Pages the setup can fill in itself may be skipped. The game pages check your pick before Next. They say what is wrong with it.
4. **Let it build.** With "Build now" ticked (the default), the setup lifts and compiles the game's code behind one progress bar. Otherwise the first start of the game does it. It takes about 10 minutes on a fast PC, and longer on slower ones. If a check fails, the message names the input and the path. It says what was found there and what it must hold. The full log is next to the installer.
5. **If the build stops** (you close it, or the PC restarts), start the game again. The build resumes at the first step that did not finish.
6. **Keep it local and fast.** Install on a local drive. An SSD helps. Do not use a OneDrive folder, because it would sync the build's thousands of files while they are written. Keep your game files on this device too. In OneDrive, choose "Always keep on this device". The setup refuses a OneDrive install folder. The first-run check refuses game files that are only online.
7. **Always start the game through the launcher** (below).

Always start the game through the launcher: **`Project Helghast.exe`**, or the desktop and Start-menu shortcuts.
`kz3.exe` started directly skips the launcher's work. It does not apply the data-location rules, so saves, settings and caches would land in the wrong folder. It does not apply your launch settings. It does not check that your install is current.

## Portable zip quick start

1. **Have your three inputs ready** (step 1 above).
2. **Unpack the whole zip** to a local folder (not OneDrive). Keep its folders: `run.cmd` needs the `toolchain` and `kz3-setup` folders beside it. The zip holds one folder, `Project Helghast`.
3. **Run `run.cmd`** in that folder (double-click it). It asks for the disc dump, the 1.14 update folder and the decrypted EBOOT, and checks them. It then offers the optional choices: the PS3 fonts, the version check and the shader warm-up.
4. **Let it build.** It lifts and compiles the game's code behind one progress bar in that window, in about 10 minutes on a fast PC. It ends with "Done in ...: the port is built." The log, `Project-Helghast-firstrun.log`, is written next to `run.cmd`.
5. **If the build stops**, run `run.cmd` again. It resumes at the first step that did not finish.
6. **Start the game with `Project Helghast.exe`** in the same folder, now and every time after.

Saves and settings stay in that folder. Nothing goes online unless you turn the version check on ([PRIVACY.md](PRIVACY.md)).

## Which file starts the game

- **Setup install:** `Project Helghast.exe`, or the desktop and Start-menu shortcuts that point to it. The installed folder has no `run.cmd`, because the setup does its work.
- **Portable zip:** `run.cmd` the first time, because it asks for your files and builds the game. After the build, `Project Helghast.exe` in the same folder starts the game. `run.cmd` is not the way to play.
- In both cases never start `kz3.exe` directly (see above).

## Requirements

For the short list, see [requirements.md](requirements.md). [Getting your files ready](GETTING-FILES.md) says what each folder must hold.

- The US disc (BCUS98234) as a decrypted dump. It can be a folder (`PS3_GAME`) or an ISO file.
- Other regions: the EU disc (BCES01007), the Asian disc (BCAS25008) and the Japanese disc (BCJS30066) are accepted as untested regions. Other releases are not supported yet.
- The 1.14 update: one from any of the four regions can go with your disc. See [requirements.md](requirements.md).
- Split archives: a dump whose `data_ps3.psarc` is cut into `.66600`, `.66601`, ... parts (a FAT32 drive) works as it is.
- The disc check: the setup checks the disc archive against the known one (our copy's). This takes about half a minute.
- The decrypted EBOOT of your own copy. No firmware is needed. This project does not provide decryption tools or instructions. The PS3 emulator RPCS3 and its documentation are the usual place people start.
- A Direct3D 12 GPU.

Hardware figures are provisional until the game runs at speed.

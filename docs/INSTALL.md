# Installing on Windows

Get the installer or the portable zip from the [releases page](https://github.com/undecuple/project-helghast/releases) and check it against `SHA256SUMS`. No game code, assets or keys are ever shipped or downloaded: the setup builds the game's code on your own machine from your own copy, with a bundled compiler. What a release must meet to be called playable is in [RELEASE-CRITERIA.md](RELEASE-CRITERIA.md).

## Windows quick start

1. **Have your three inputs ready**, all from your own copy of the game
   ([GETTING-FILES.md](GETTING-FILES.md) says what each must hold):
   - the disc dump: the folder that holds `PS3_GAME`, or a decrypted `.iso` file;
   - the 1.14 update folder (the one that holds `PARAM.SFO` and `USRDIR`);
   - the decrypted 1.14 EBOOT, `EBOOT.elf`.
2. **Run `Project-Helghast-setup.exe`.** It installs for your user only and needs no administrator rights.
3. **Answer its pages:** the setup type, the install folder, the tasks (desktop shortcut, "Build now"), the disc dump
   (a folder, or "Browse for ISO"), the 1.14 update folder, the decrypted EBOOT, and optionally the PS3 fonts and the
   advanced options. Pages the setup can fill in itself may be skipped. The game pages check your pick before Next
   and say what is wrong with it.
4. **Let it build.** With "Build now" ticked (the default) the setup lifts and compiles the game's code behind one
   progress bar; otherwise the first start of the game does it. It takes about 10 minutes on a fast PC, longer on
   slower ones. If a check fails, the message says which input, the path, what was found
   there and what it must hold; the full log is next to the installer.
5. **If the build stops** (you close it, or the PC restarts), start the game again: the build resumes at the first
   step that did not finish.
6. **Keep it local and fast:** install on a local drive (an SSD helps), not in a OneDrive folder (it would sync the
   build's thousands of files while they are written), and keep your game files on this device (in OneDrive:
   "Always keep on this device"). The setup refuses a OneDrive install folder, and the first-run check refuses game
   files that are only online.
7. **Always start the game through the launcher** (below).

**Always start the game through the launcher (`Project Helghast.exe`, or the desktop and Start-menu shortcuts).**
`kz3.exe` started directly skips the launcher's work: it does not apply the data-location rules (saves, settings and
caches would land in the wrong folder) or your launch settings, and it does not check that your install is current.

## Requirements

See [requirements.md](requirements.md) (and [Getting your files ready](GETTING-FILES.md) for what each folder must hold): the US disc (BCUS98234) as a decrypted dump, a folder (PS3_GAME) or an ISO file (the EU disc BCES01007, the Asian disc BCAS25008 and the Japanese disc BCJS30066 are accepted as untested regions; other releases are not supported yet; a 1.14 update from any of the four regions can go with your disc, see [requirements.md](requirements.md)). A dump whose data_ps3.psarc is cut into .66600, .66601, ... parts (a FAT32 drive) works as it is. The setup checks the disc archive against the known one (our copy's); about half a minute, the 1.14 update,
the decrypted EBOOT of your own copy (no firmware needed; this project doesn't provide decryption tools or instructions; the PS3 emulator RPCS3 and its documentation are the usual place people start), and a Direct3D 12 GPU. Hardware figures are provisional until
the game runs at speed.

# Getting your files ready

The setup needs three things from your own copy of Killzone 3: the disc dump, the 1.14 update folder and the decrypted
EBOOT. This page says what each one must look like and what the setup tells you when something is off. It does not
say how to make the files. For the update and the decrypted EBOOT, the documentation of the PS3 emulator RPCS3 is the
place to read. Nothing here is shipped or downloaded by this project.

## The three inputs

| Input | What to pick in the setup | The folder or file must contain | Also accepted | Quick self-check |
|---|---|---|---|---|
| Disc dump | A folder dump: the folder that holds `PS3_GAME`. Or a decrypted disc ISO: the `.iso` file, picked as a file. Not a drive root such as `E:\` unless `PS3_GAME` sits directly on it | `PS3_GAME/PARAM.SFO`, `PS3_GAME/USRDIR/EBOOT.BIN`, and the game archive `PS3_GAME/USRDIR/data_ps3.psarc` (or its parts `data_ps3.psarc.66600`, `.66601`, ... with no gap) | The `PS3_GAME` folder itself, or `PS3_GAME/USRDIR`, picked one or two levels too deep (the setup steps up to the folder above `PS3_GAME`) | Open the folder: you should see `PS3_GAME`, and in it `PARAM.SFO` and `USRDIR`. The archive is about 9.6 GB |
| 1.14 update | The update folder | `PARAM.SFO`, `USRDIR/EBOOT.BIN` and `USRDIR/added_data_ps3.psarc.edat` | Its `USRDIR` folder picked by mistake (the setup steps up to the folder above) | Open the folder: `PARAM.SFO` sits next to `USRDIR`. Its `APP_VER` must be 01.14 |
| Decrypted 1.14 EBOOT | The file `EBOOT.elf` made from the 1.14 update's `USRDIR/EBOOT.BIN` (in the update folder, for example `dev_hdd0/game/BCUS98234/USRDIR`), not the disc's `PS3_GAME/USRDIR/EBOOT.BIN` | A 64-bit PowerPC ELF file, not the encrypted `EBOOT.BIN`. The disc's EBOOT is version 1.00: the setup refuses it on the EBOOT page and the first build stops on it (E1) | If `USRDIR/EBOOT.elf` is already inside the update folder, the setup uses it and does not ask. When the console asks for it, give the file or the folder that holds it, or press Enter to use the path shown in brackets | The file starts with the letters `ELF`; an encrypted file starts with `SCE`. Its size is 21,228,256 bytes for 1.14 (the disc's 1.00 one is 20,745,952 bytes) |

A decrypted ISO is read in place by the first-run check; its files are copied into the install folder once. An
encrypted ISO is refused.

The setup checks these folders and then builds the port. If you did not tick "Build now", or a check failed, the
first start of the game shows the check in a console window and lets you fix the problem or change the folders; it
starts from the folders you gave in the setup, so you are not asked for them again.

## Accepted layouts and regions

- **Folder dump.** `PS3_GAME` with its files, as above.
- **Split archive.** A dump on a FAT32 drive often holds `data_ps3.psarc.66600`, `.66601`, ... instead of one
  `data_ps3.psarc`. The setup accepts this as it is. The numbers must run on without a gap; a missing part is reported.
- **Decrypted ISO.** One `.iso` file. Pick the file, not a folder.
- **Regions.** The US disc (BCUS98234) is the tested one. The EU disc (BCES01007), the Asian disc (BCAS25008) and the
  Japanese disc (BCJS30066) are accepted as untested regions: the setup goes on and shows a note. Other titles are
  refused.
- **Mixed regions.** A 1.14 update from any of the four regions can go with a disc from any of them. In our tests
  with US and EU discs the update gave the same game data. The setup shows a note and goes on.

## What the first-run check can print

The first step of the build prints one line per check: `OK`, `WARN` or `FAIL`, then a code. A `FAIL` stops the setup.
A `WARN` lets it go on. The full log is next to the installer.

| Code | Level | What it means | What to do |
|---|---|---|---|
| D0 | OK / FAIL | OK: the disc is a decrypted ISO. FAIL: the dump folder was not found | Pick the dump folder or ISO again |
| D1 | OK / WARN / FAIL | FAIL: `PARAM.SFO` is missing or unreadable, or its title id is not Killzone 3. WARN: an untested region, or a folder whose category is not a disc game (maybe an installed copy) | Pick the folder that holds `PS3_GAME`. For a WARN you can go on |
| D2 | OK / WARN / FAIL | FAIL: `PS3_GAME/USRDIR/EBOOT.BIN` is missing, or is neither a SELF nor an ELF file. WARN: an unusual size | Copy or dump the disc again if it is a FAIL |
| D3 | OK / FAIL | The game archive is missing, is too small, does not start with `PSAR` (not an archive, or still encrypted), or its parts have a gap | Copy or dump the disc again; check that all parts `.66600`, `.66601`, ... are there |
| A1 | OK / WARN / FAIL | The archive is compared with the known disc archive. OK: it matches. FAIL: it differs (damaged or changed copy). WARN: the archive is not known yet (untested region), or could not be compared | A FAIL stops the build: copy or dump the disc again. If a fresh copy of a US or EU disc still fails, open an issue with the first-run log (the known archive is the one on our copy). The check takes about half a minute |
| I1 | FAIL | The ISO is encrypted | Use a decrypted dump |
| I2 | FAIL | The ISO cannot be read | Check that the file is complete and is a disc image |
| U0 | WARN / FAIL | No update folder was given (WARN), or it was not found (FAIL) | Pick the update folder |
| U1 | OK / WARN / FAIL | FAIL: the update's `PARAM.SFO` is missing or is not Killzone 3, or it is the disc's (the disc's `PS3_GAME` folder was given as the update). WARN: an untested region, an update from another region than the disc, or a version other than 01.14 | Pick the right update. A mixed-region WARN can be ignored |
| U2 | OK / FAIL | `USRDIR/EBOOT.BIN` is missing in the update folder | Pick the folder that holds `PARAM.SFO` and `USRDIR` |
| U3 | OK / WARN | `USRDIR/added_data_ps3.psarc.edat` is missing: the update's data layer would be missing | Use a complete update folder |
| E1 | OK / WARN / FAIL | FAIL: no `EBOOT.elf` was found, it is not a 64-bit PowerPC ELF, or it is the disc's 1.00 EBOOT (or another known version). WARN: an unknown size | Pick the decrypted EBOOT of the 1.14 update, from the update's `USRDIR` folder |
| H1 | OK / WARN | The EBOOT is compared with the known 1.14 one. A WARN means it differs or could not be compared | The build may still work if it is a 1.14 EBOOT; if the build fails, check the file |
| O1 | FAIL | The install folder or the build folder is inside OneDrive | Choose a folder outside OneDrive and run the setup again |
| O2 | FAIL | A game file exists only online in OneDrive | See the OneDrive section below |
| O3 | OK | Information only: a game folder is inside OneDrive but its files are on this device | Nothing |

When a check fails, the setup's message box and the console show one line per input that failed, labelled
`Disc dump`, `1.14 update` or `EBOOT`: the path it looked at, what is there, and what that folder or file must hold.
They also say when the folder looks like the other input (the update given as the disc, or the disc as the update),
and, for a drive root, which folders on its top two levels look like the disc dump or the update. A last line says
which inputs passed.

## OneDrive

- **Install folder:** keep it outside OneDrive (O1). The build writes thousands of files, and OneDrive would sync them
  while they are written and lock some of them. The installer's default folder is fine.
- **Game files:** they may be in OneDrive only when they are kept on this device (O2 and O3). In File Explorer, right-click
  the game folder (the disc dump, the update and the EBOOT's folder), choose "Always keep on this device", wait until
  OneDrive has downloaded it, then run the setup again. The check looks before it reads anything, so it does not start
  a download.

## More

- [Building the port](BUILDING.md) lists what the whole build needs.
- The [README](../README.md#requirements) has the requirements in short.

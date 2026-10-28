# Release criteria

Project Helghast has no release and no release date. Versions below 1.0.0 are previews: **v0.x = preview**, published
as GitHub pre-releases, each saying what is missing. **v1.0.0 = the whole campaign playable start to finish at about the
game's own 30 frames per second.** The first release that calls itself **playable** waits for every point of the second
list. Booting to a title screen, or to a black level, does not count. This list, not a date, is what a release is
measured against.

## Gates for a v0.x tag (preview)

1. **CI green** on the commit that is tagged.
2. **The release workflow builds** the installer and the portable zip from that commit.
3. **Installer and portable zip tested in a Windows VM:** install, the first-run checks, the file layout, uninstall.
4. **Known issues listed** in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).
5. **SHA256SUMS** published beside the assets.
6. **One version string** in the exe, the launcher, the installer and `CITATION.cff` (checked by
   `tools/release/test_version_agree.py`).
7. **Tags are never moved**; a fix is a new tag.
8. **Published as a pre-release** while the version is 0.x.

## Release criteria for v1.0.0 (playable)

1. **Played start to finish by a person:** the whole campaign on one save, by hand, with a dated playthrough log
   (chapter, time, build, bugs met) published beside the release.
2. **A picture first:** the game's frames are drawn correctly in every chapter and compared with the PS3 emulator
   RPCS3 on the same scene.
3. **Automated coverage:** a scripted sweep that loads and plays every chapter on the release build, longer runs per
   chapter logging asserts, stalls and frame rate, checkpoint save and reload, quit and continue, and the boot check.
4. **Speed:** about 30 frames per second in levels on the recommended hardware (see
   [requirements.md](requirements.md)), at least what RPCS3 reaches on the same machine and scene, with steady frame
   pacing (1 % lows and frame-time deviation measured next to the average). Numbers go in the release notes.
5. **No known crash or soft-lock** on the campaign path; everything else in the published known-issues list.
6. **Accurate label:** the README and release notes say what is translated, what is reimplemented and what is native,
   and how AI was used and to what degree (README, "How AI was used").
7. **Something over the original and the emulator, documented:** the port-only rows of [FEATURES.md](FEATURES.md),
   each checked.
8. **Getting the game files explained:** the installer needs the files of your own copy of the game: the disc as a
   folder (its `PARAM.SFO` and the `PS3_GAME` layout), the 1.14 update, and the decrypted `EBOOT` of that copy. It
   checks the serial and the file sizes and magics, and refuses anything else with a message saying which file is
   wrong. Where to start for making those files: the PS3 emulator RPCS3 and its documentation. This project gives no
   decryption procedure or keys, and does not link to or name where to get game files.
9. **Maintenance shown, not promised:** a changelog, GitHub releases, and an issue template that asks for GPU, driver
   and the newest log file.
10. **No emulator tells:** release builds expose none of the runtime's debug UI, hotkeys, overlays or diagnostic
    switches by default; only the port's own settings, styled like the game.

The numbers here are targets for 1.0.0, not a statement about today: see the README's [Status](../README.md#status)
table for what works now.

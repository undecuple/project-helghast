# Known issues

What is known to be wrong in the port today, as seen in the project's own test sessions. Each entry says what you see,
where, whether there is a way around it, and its status. "Last seen" is the date of the most recent observation we
recorded, with the build in use; it is not a promise that the problem is still there on a newer build. The overall
picture is in the [README status table](../README.md#status); what is planned is in the [roadmap](ROADMAP.md); what a release must meet before it is called playable is in [RELEASE-CRITERIA.md](RELEASE-CRITERIA.md).

Found something not listed here? Please open an issue with the [bug report form](../.github/ISSUE_TEMPLATE/bug_report.yml)
(or the performance form for speed and stutter), and see [Contributing](../README.md#contributing). No issues are
linked below yet because none has been filed for these; they are tracked in the project's own notes.

| Problem | Where it shows | Workaround | Status |
|---|---|---|---|
| [Frame rate about 15-20 in levels](#frame-rate) | in levels, both Proton and native Windows | none | open |
| [Audio stutters now and then](#audio-stutter) | in levels | none known | open |
| [Windows and sky in far views look flat white](#flat-white) | far views in levels | none | open |
| [Keyboard input intermittent under Steam/Proton](#keyboard-proton) | Steam with Proton | use a controller | open, cause not confirmed |
| [Botzone: no full match played](#botzone) | offline Botzone matches | none needed to reach Career Selection | open |
| [Split-screen: views offset, right view can flicker](#split-screen) | local two-player co-op | none | open |
| [Co-op needs two controllers](#co-op-pads) | local two-player co-op | use two controllers | open (keyboard and mouse as player 2 is planned) |
| [The mouse does nothing in some dialogs](#mouse-dialogs) | confirm dialogs, for example "Exit Killzone 3?" | keyboard or controller | open |
| [Mouse look feels jumpy](#mouse-jumpy) | on foot | `KZ3_MOUSE_LATCH=0` switches the view to the game's own timing | open (hand check pending) |
| [A black level after skipping the closing movie](#skip-movie) | after the prologue | do not skip the movie | open |
| [Controller settings page overlapped](#controller-page) | Options, controller page | none | rows on their own page; hand check pending |
| ["Modern" controls: effect in a level not confirmed](#modern-controls) | in levels, modern layout | use the game's own layout | to be confirmed |

## Frame rate

Levels run at about 16-23 frames per second, where the game's own target is 30. One measured 50-second window of Frozen
Shores (131-180 s after launch) runs at about 22-23 frames/s (means of alternated runs, 2026-10-27; in-level figures, not
menus); the slowest 1 % of frames run at about 13-14 frames/s. A loading screen before a level sometimes stalls for a few
seconds; the cause found is the video decoder threads running on efficiency cores of hybrid CPUs, so they are kept on
performance cores (`KZ3_SPU_PCORES=0` turns that off). No frame-rate effect was measured and the stall itself was not
seen in those runs, so it is not called fixed. The first visit to a level is slower while shaders build (the README's
performance row has the numbers). The limits are the renderer's command processing and the SPU job code; the PS3
emulator RPCS3 holds 30 on the same scene and machine, so 30 is reachable. Split-screen co-op runs at a mean of about
26 frames/s (2026-10-22). Status: open, being worked on.

## Audio stutter

Sound breaks up now and then during levels. Cause found: the audio mixing code runs about 1.2 % short of real time; that gap is what you hear. Last seen: 2026-10-22. No workaround.
Status: open.

## Flat white

Windows and sky in far views are drawn flat white instead of with their normal colours. The likely cause is the tone and
grading data the far view uses; it is not fixed. Last seen: 2026-10-22. No workaround. Status: open.

## Keyboard under Steam/Proton

The keyboard sometimes stops working, intermittently, when the game is started through Steam with Proton. Window focus is
suspected but not confirmed. A controller is not affected by this. Last seen: 2026-10-22. Status: open.

## Botzone

With the game's own patch layer applying the 1.14 update (`KZ3_NATIVE_PATCH`, on by default), Botzone reaches Career
Selection (Corinth Highway) and the campaign levels 200-208 all load (scripted runs, 2026-10-27). A full Botzone match
has not been played by hand. Hand sessions on 2026-10-22 under Proton stopped on the "syncing" screen; one on native
Windows (2026-10-21) reached the spawn screen. Status: open.

## Split-screen

In the game's own local two-player co-op the two views sit slightly out of place: one is shifted up and the other down,
by about 80 pixels of the game's 1280x720 picture. On one later build the right-hand view also flickered (it did not
on the first build tested). Both were last seen 2026-10-22, under Proton. No workaround. Status: open, under investigation.

## Co-op pads

The game's own two-player co-op checks for two controllers and shows "Controller for Second Player is missing" when it
sees only one. With two controllers connected (and Steam Input off for the game) it works: first played 2026-10-22.
Using the keyboard and mouse as the second player is not built yet. Status: open.

## Mouse dialogs

The mouse works in the menu lists but does nothing in some dialogs, such as the "Exit Killzone 3?" Yes/No box and the
co-op controller message. Use the keyboard or a controller there. Last seen: 2026-10-22. Status: open.

## Mouse jumpy

Mouse look on foot turns the view but feels jumpy: the movement per frame alternated between two sizes (play-test 2026-10-25).
Look input is cut on the game's own simulation clock and every mouse count is shown on the view at once (about 0 ms from
counts to the picture). Where the shots go follows the game's 30 Hz simulation, about 35 ms behind the view; mounted guns
follow the mouse within the gun's range, with the simulation's look timing. `KZ3_MOUSE_LATCH=0` switches the view to the
game's own timing. The feel is not yet checked by hand. Status: open.

## Skip movie

Skipping the prologue's closing movie leaves the next level black, with only the on-screen display drawn, although the
game keeps running. Last seen: 2026-10-22. Watching the movie to the end avoids it (not re-tested on later builds).
Status: open.

## Controller page

On the Options controller page, rows added by the port overlapped the picture of the controller and its labels (seen
2026-10-22 and 2026-10-25). The port's rows are on their own page (Options > Controls > Advanced
Settings > Layout and Prompts) and the controller page is the game's own; this was checked in screenshots, not by
hand. Status: hand check pending.

## Modern controls

With the "modern" layout the Back button showed Square instead of Circle in menus (play-test, 2026-10-25). The
layout is applied inside the game's own action table and menus keep Cross to confirm and Circle to go back
(checked in the Options screens in one run). What is not confirmed is how the modern layout behaves in a level: in one
level with the Modern layout (Killzone Shadow Fall style) fire, reload, jump and aim worked on the new buttons (2026-10-27); Modern keeps Use on Circle (the Shadow Fall layout) and the controller picture shows no L1 line under Modern; what has not been checked is Use at a use prompt and the rest of the buttons. The Custom layout (per action, 33 groups) has not been played in a level yet and capture from a real pad is unchecked. Status: to be confirmed; the game's own layout is the default.

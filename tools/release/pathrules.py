#!/usr/bin/env python3
"""The one path-redaction rule of every log the player may post (2026-11-02): the launcher log, the install
(first-run) log with the merged Inno log, the support package, the crash report. The C twin is port/kz3/kz3_log_redact.h
(the game's log); the Pascal fallback is installer/kz3.iss RedactPaths. All three keep the same cases (see the tests).

The rule, in the order it is applied (a folder is named by the first rule that finds it):
  1. the install folder              -> %INSTALL%   (the rest of the path stays)
  2. %LOCALAPPDATA% / %APPDATA%      -> keep their names (the rest stays)
  3. the user's home                 -> %USERPROFILE% (firstrun.redactor, the home forms)
  4. every other absolute path       -> <path>\\ + its LAST component only. Absolute = a drive (X:\\ or X:/, any case,
     doubled backslashes, %3A/%5C/%2F forms, CMake's mangled X_/), a UNC path (\\\\server\\share\\...), or a Unix path
     with at least two components (/home/..., /mnt/..., Proton's Z:\\home\\...).
     Unchanged: relative paths and the game's own virtual paths (/dev_bdvd/..., /dev_hdd0/..., /app_home/...).
A folder name may hold spaces and parentheses ("D:\\Program Files (x86)\\Killzone 3\\x.exe"): the folders in the middle
may; the last component ends at white space, so a name with a space leaves only its first word of the last component
(never more of the path). A path the rule misses is a bug."""
import re

SEP = r"(?:\\{1,2}|/|%5C|%2F)"
# folders in the middle of a path: anything up to the next separator, but not across a quote, '|', '*', '?', ':', a tab,
# a line end, or a space that starts the next Unix path (" /")
_MID = r"(?:(?!" + SEP + r")(?! /)[^\r\n\"'|*?:\t])*"
_LAST = r"((?:(?!" + SEP + r")[^\s\"'|*?:,;)])*)"
# virtual paths of the game (and the PS3 file system): not folders of the player's machine
_VIRTUAL = r"(?!(?:dev_|app_|host_root))"

PATH_RX = re.compile(
    # drive: C:\ C:/ C:\\ C%3A%5C and CMake's C_/ ; the letter is not the end of a longer word
    r"(?<![\w])(?:[A-Za-z](?::|%3A)" + SEP + r"|[A-Za-z]_/)(?:" + _MID + SEP + r")*" + _LAST
    # UNC: \\server\share\...
    + r"|(?<![\w\\/.:%\-])\\\\[^\\/\s\"'|*?:]+" + SEP + r"(?:" + _MID + SEP + r")*" + _LAST
    # Unix: /a/b ... after white space, a quote, '=', '(' '[' ',' ';' '{', the start, or file://
    + r"|(?:(?<=file://)|(?<![^\s\"'=(\[,;{]))/" + _VIRTUAL + r"(?:" + _MID + SEP + r")+" + _LAST,
    re.IGNORECASE)


def redact_paths(s):
    """Rule 4 on one text: every absolute path becomes <path>\\<last component>."""
    return PATH_RX.sub(lambda m: "<path>\\" + next(g for g in reversed(m.groups()) if g is not None), s)

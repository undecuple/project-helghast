"""The product's public name (Project Helghast: the Helghast are Killzone 3's own faction; the project is not
official), file names and links in one place (release pass 19; plan 4.18).

Every release tool (launch.py, firstrun.py, version_check.py, pack_zip.py, assemble_payload.py, installer_art.py,
website.py) takes its names from here, so a rename is a change of the constants below plus the mirrors this module
cannot reach, which tools/release/test_names.py checks against it:
  installer/kz3.iss          the #define block (AppName, AppDirName, ProjectURL, SiteURL, SiteText, LauncherExe,
                             SetupBase, OldDataDirName)
  installer/launcher/*.c/.rc the launcher's window title, issues link and version strings
  installer/launcher/launcher.rc, port/kz3/CMakeLists.txt (-> app.rc), installer/kz3.iss, public/CITATION.cff
                             the VERSION mirrors (test_version_agree.py)
  installer/run.cmd          the first-run log name
  .github/workflows/release.yml  the setup file name
  public/ (README, NOTICE, CITATION, .recomp.json, BUILDING, issue templates) and the art/site outputs (regenerate:
  installer_art.py, website.py).
Internal names stay as they are on a rename: kz3.exe, the KZ3_* switches, the kz3-* tools, the Inno AppId,
the runtime's Killzone3-PC- log prefix. Shipped in the payload next to launch.py (assemble_payload.py FILES).
"""

VERSION = "0.1.0"                            # the release version; tools/release/test_version_agree.py checks every mirror
PRODUCT = "Project Helghast"                    # every title a player reads (installer, launcher, site, art)
TAG = "Killzone 3 // Unofficial PC Port"          # the "GAME // UNOFFICIAL PC PORT" tag (art: upper case)
DESCRIPTION = f"{TAG} (static recompilation)"    # the findable description line (pass 21: carries the tag)
WORDMARK = ("PROJECT", "HELGHAST")              # the art's wordmark: white word, red word (no monogram)
FILE_BASE = "Project-Helghast"                   # setup.exe, logs and zip names: FILE_BASE-setup.exe, ...
OWNER = "undecuple"
REPO_SLUG = "project-helghast"                   # github.com/OWNER/REPO_SLUG (GitHub redirects a renamed repository)

REPO_URL = f"https://github.com/{OWNER}/{REPO_SLUG}"
ISSUES_URL = f"{REPO_URL}/issues"
SITE_TEXT = f"{OWNER}.github.io/{REPO_SLUG}"
SITE_URL = f"https://{SITE_TEXT}"
API_RELEASES = f"https://api.github.com/repos/{OWNER}/{REPO_SLUG}/releases?per_page=10"   # the list, not releases/latest (which hides pre-releases)
USER_AGENT = f"{REPO_SLUG}-version-check"
CREDIT = f"{PRODUCT} by {OWNER}"
NAME_NOTE = ("\"Helghast\" is the name of the game's own faction; Project Helghast is a fan project, not affiliated "
             "with or endorsed by Sony Interactive Entertainment or Guerrilla Games.")   # site Legal

LAUNCHER_EXE = f"{PRODUCT}.exe"
SETUP_BASE = f"{FILE_BASE}-setup"            # Inno OutputBaseFilename; the setup's log is SETUP_BASE.log
SETUP_LOG = f"{SETUP_BASE}.log"
FIRSTRUN_LOG = f"{FILE_BASE}-firstrun.log"
ZIP_TOP = PRODUCT                            # the portable zip's one top folder


def zip_name(version):
    return f"{FILE_BASE}-{version}-portable.zip"


# %APPDATA% data folder (saves, keybinds.ini, online.ini, caches) and the names it had before. launch.py moves an
# old folder's contents to DATA_FOLDER once (migrate_data); the uninstaller offers to delete each of them.
# ASSUMPTION: the runtime (port/net/kz3_net.c online.ini, patch 2037 keybinds.ini) names the same folder; a rename
# changes those strings too (outside this module: a maintainer request).
DATA_FOLDER = PRODUCT
OLD_DATA_FOLDERS = ("Killzone 3 PC",)        # the name before the rename to Project Helghast (2026-10-12)

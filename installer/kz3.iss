; Project Helghast: Inno Setup 6 script.
;
; Compiled with ISCC 6.7.3 under Proton and every wizard page walked headless
; (tools/release/wizard_test.sh, payload without the large toolchain parts, a demo build step). Run on native Windows
; 11 with the full toolchain payload on 2026-10-21 (the install built the game in 422 s and the game reached the menus);
; the wizard pages were walked headless under Proton only. Needs the release payload (kz3-setup\, ps3recomp\, port\,
; toolchain\, licenses\, the launcher "Project Helghast.exe") and the headings font (FontDir, below).
;
;   iscc /DPayload=<payload dir> /DAppVersion=<x.y.z> installer\kz3.iss      (output: installer\Output\)
;
; What it does: per-user install (no admin) into a folder the player picks; three pages for the player's own dump,
; 1.14 update and decrypted EBOOT (quick presence checks here; tools/release/firstrun_check.py runs the full check
; as the build's first step); runs tools/release/firstrun.py with one progress bar; desktop shortcut (checked by
; default), Start-menu entry and "Project page" link; a "portable install" option (saves and settings in the
; install folder); one install log next to setup.exe with Inno's own log merged in; an uninstaller that asks before
; deleting saves (default keep). It downloads nothing and copies nothing of the game unless the player ticks "Copy the
; game files into the install" on the Advanced page. The welcome
; page's "Check now" runs tools/release/version_check.py once, only on that click: one anonymous GET of
; the latest-release JSON; a newer release opens its page in the browser only on a second click; offline = one line
; in the log. The finish page and the build-error dialog offer "Open log" and "Report a problem" (the issues page).
;
; Silent installs: /VERYSILENT /SUPPRESSMSGBOXES [/DUMP=<dump> /UPDATE=<update folder> [/EBOOT=<EBOOT.elf>]].
; The game folders come from these parameters, else the earlier install's recorded folders, else its build\paths.json;
; none known = the files are installed and the first start of the launcher asks for them (one log line, exit code 0);
; a folder given or recorded that fails the presence check, or only some of them, = nothing installed, one log line,
; exit code 7. A silent build that does not finish logs one line and still exits 0 (the RESULT line is in the log).
; Setup types: Recommended asks only for the dump and the update (install folder aside); the EBOOT
; page shows only when <update>\USRDIR\EBOOT.elf is missing, the fonts come from RPCS3's folder beside the update
; when it has them, the tasks keep their defaults. Advanced adds the tasks page, the EBOOT and fonts pages and the
; "Advanced options" page (copy the game files, crash reports, font copy, FMA, window size, language). Silent:
; /TYPE=advanced /COPYDATA=1 /CRASHREPORTS=1 /FONTCOPY=0 /FMA=auto|on|off /LOWMEM=auto|on|off /WINDOW=<size> /LANG=<code>.
; Look: wizard images and icons from installer\art\ (tools/release/installer_art.py, the
; site's palette and fonts; our own artwork); native controls, no skin add-on. Texts in the site's voice: short, direct.

#ifndef AppVersion
  #define AppVersion "0.1.1"
#endif
#ifndef Payload
  ; ASSUMPTION: where the payload assembly will put the tree
  #define Payload "..\build\release-payload"
#endif
#ifndef FontDir
  ; Rajdhani Bold for the headings: fetched and hash-checked against installer/art/art.json
  ; by tools/release/installer_art.py --fetch-fonts (SIL OFL 1.1); never committed
  #define FontDir "..\work\release\fonts"
#endif
; Crash reports (opt-in, Advanced page): the address they go to comes from the build
; environment (the release workflow's KZ3_SENTRY_DSN secret), never from the repository. Empty = this setup cannot
; send any, and the box is greyed out. Nothing sends yet: the runtime side (sentry-native) is a later step.
#ifndef CrashDSN
  #define CrashDSN GetEnv("KZ3_SENTRY_DSN")
#endif
#if VER < EncodeVer(6,6,0)
  #error Inno Setup 6.6.0 or later is needed (WizardStyle dark; tested with 6.7.3)
#endif
; The licence page shows LICENSE with each paragraph on one line: the file is hard-wrapped at about 72 columns and
; the memo wraps again at its own width, which left one or two words on every other line (wizard test 2026-10-12).
; Done here at compile time, so setup loads the file the normal way (a runtime Text reset lost the dark style's text
; colour). Written to %TEMP% because the script folder may be read-only.
#define LicSrc Payload + "\licenses\LICENSE.txt"
#define LicOut AddBackslash(GetEnv("TEMP")) + "kz3-wizard-licence.txt"
#define LicText ""
#define LicLine ""
#define LicPara 1
#define LicH 0
#sub LicAdd
  #define LicLine = Trim(FileRead(LicH))
  #if LicLine == ""
    #if !LicPara
      #define public LicText = LicText + NewLine + NewLine
    #endif
    #define public LicPara = 1
  #else
    #define public LicText = LicText + (LicPara ? "" : " ") + LicLine
    #define public LicPara = 0
  #endif
#endsub
#for {LicH = FileOpen(LicSrc); LicH && !FileEof(LicH); ""} LicAdd
#if LicH
  #expr FileClose(LicH)
#else
  #error cannot read the licence: {#LicSrc}
#endif
#expr SaveStringToFile(LicOut, LicText, 0, 1)
; One product name on every screen (UI review 12, docs/guides/ui-style.md); AppId below never changes.
; Names block: mirrors tools/release/names.py (PRODUCT, DATA_FOLDER, OLD_DATA_FOLDERS, REPO_URL, SITE_URL, SITE_TEXT,
; LAUNCHER_EXE, SETUP_BASE); tools/release/test_names.py fails when they differ. A rename changes both.
#define AppName "Project Helghast"
; the description line (tools/release/names.py DESCRIPTION); a #define so the "//" sits inside a quoted string
#define AppDescription "Killzone 3 // Unofficial PC Port (static recompilation)"
#define AppDirName "Project Helghast"
; The data folder's (%APPDATA%) earlier name after a rename (names.OLD_DATA_FOLDERS[0]), "" = none: the uninstaller
; then also offers that folder, and an install over an install of the old name removes its launcher and shortcuts
#define OldDataDirName "Killzone 3 PC"
; ProjectURL = the GitHub repository (issues, releases, source); SiteURL = the project page shown as text and
; opened by "Project page" (the github.io site: live only from the public flip)
#define ProjectURL "https://github.com/undecuple/project-helghast"
#define SiteURL "https://undecuple.github.io/project-helghast"
#define SiteText "undecuple.github.io/project-helghast"
#define LauncherExe "Project Helghast.exe"
#define FileBase "Project-Helghast"
#define SetupBase "Project-Helghast-setup"

[Setup]
AppId={{08141EE8-C3FD-416B-9F09-5977CFF2F6A3}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppName} contributors
AppComments={#AppDescription}
AppPublisherURL={#SiteURL}
AppSupportURL={#ProjectURL}/issues
AppUpdatesURL={#ProjectURL}/releases
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=
DefaultDirName={localappdata}\Programs\{#AppDirName}
DisableDirPage=no
; Browse appends the app's own folder name; the Pascal code below keeps the last folder name ours (uninstall removes {app})
AppendDefaultDirName=yes
DefaultGroupName={#AppDirName}
DisableProgramGroupPage=yes
; The Start-menu folder follows the current name, not a previous install's (after a rename)
UsePreviousGroup=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
; Dark by default, not following the system. Inno 6.7.3 help (WizardStyle topic): forced dark uses
; the built-in dark style for pages, message boxes and task dialogs, and a dark native title bar where the system
; allows it; a Windows high-contrast theme (or /NOSTYLE) turns the custom style off, so high contrast still wins.
WizardStyle=modern dark
; The art's palette (installer_art.py PAL, site style.css --bg): page background #0a0909 like the wizard image and
; the README banner; controls and text keep Inno's built-in dark style (no custom style file: buttons stay its grey).
; High contrast turns the custom style off; ASSUMPTION: it also drops WizardBackColor (to check on real Windows).
#ifndef NoBackColor
WizardBackColor=#0a0909
#endif
WizardImageBackColor=#0a0909
WizardSmallImageBackColor=#0a0909
; Inno 6 hides the welcome page by default; it carries the notice and "Check now"
DisableWelcomePage=no
SetupLogging=yes
OutputBaseFilename={#SetupBase}
Compression=lzma2/max
SolidCompression=yes
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\kz3-uninstall.ico
SetupIconFile=art\kz3-setup.ico
; Inno picks the closest size per DPI (100/125/150/200 %); relative to this script's folder
WizardImageFile=art\wizard-100.bmp,art\wizard-125.bmp,art\wizard-150.bmp,art\wizard-200.bmp
WizardSmallImageFile=art\header-100.bmp,art\header-125.bmp,art\header-150.bmp,art\header-200.bmp
WizardImageStretch=yes
; Beyond the installed files (payload ~0.55 GB with the toolchain): the build folder, measured 1.1 GB in release round
; 12 (1.00 tree), plus 0.4 GB headroom (ASSUMPTION: the 1.14 tree and the shader cache;
; docs/requirements.md has 1.7 GB measured for install plus build).
; The directory page then shows about 2.0 GB, the figure README/requirements/site use (UI review 13).
ExtraDiskSpaceRequired=1610612736
LicenseFile={#LicOut}

[Messages]
; Sentence-case headings, "licence" in our prose, "Setup" as Inno writes it (UI review 18-19, ui-style.md "Case")
WizardLicense=Licence agreement
LicenseLabel3=Please read the following licence. You must accept it before continuing with the installation.
WizardSelectDir=Select destination location
WizardSelectTasks=Select additional tasks
WizardReady=Ready to install
WizardPreparing=Preparing to install
WizardUninstalling=Uninstall status
UninstallAppFullTitle=Uninstall %1
BrowseDialogTitle=Browse for folder
ButtonNewFolder=&Make new folder
DirExistsTitle=Folder exists
DirDoesntExistTitle=Folder does not exist
WelcomeLabel1={#AppName} {#AppVersion}
WelcomeLabel2={#AppDescription}, built on your PC from your own game.%n%nThis installs the port's sources, the patched ps3recomp toolkit and a compiler. Then it checks your Killzone 3 disc dump (BCUS98234), the 1.14 update and the decrypted EBOOT, and builds the game here: 5 to 30 minutes.%n%nNothing is downloaded. Nothing of the game is copied unless you choose that (Advanced). What it builds holds the game's code in translated form: keep it to yourself.%n%nProject page: {#SiteText}
FinishedHeadingLabel=Setup is done
FinishedLabel={#AppName} is installed. Start it from the desktop or the Start menu.
FinishedLabelNoIcons={#AppName} is installed.
SelectDirLabel3={#AppName} installs into a "{#AppDirName}" folder inside the location you choose. Click Next to continue, or Browse to choose another location.
SelectDirBrowseLabel=To continue, click Next.
SelectTasksLabel2=Pick what Setup does after copying the files. Everything online is off unless you tick it.

[CustomMessages]
; The tasks list has no spacing property: a line break before each later group label gives the gap (UI review 20)
; The shortcut and the portable choice share one "Install:" group, so the whole list (the
; opt-in online box last) fits without scrolling at 100 % and 150 % DPI
GrpInstall=Install:
GrpBuild=%nBuild:
GrpShaders=%nShader cache:
GrpOnline=%nOnline (opt-in):

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "{cm:GrpInstall}"
Name: "portable"; Description: "&Portable: saves and settings in the install folder (else in %APPDATA%)"; GroupDescription: "{cm:GrpInstall}"; Flags: unchecked
Name: "buildnow"; Description: "&Build now (else the first start builds)"; GroupDescription: "{cm:GrpBuild}"
; The warm-up takes ~17 min the first time (the opening cinematic cannot be skipped
; on a first boot), but the session after it still compiled ~900 shaders in its first 3 minutes on the 1.00 build, so
; "while playing" is the default until a warm-up measurably shortens the next session's stalls.
Name: "shadersplay"; Description: "&Warm it while playing (no wait; the first minutes stutter while shaders compile)"; GroupDescription: "{cm:GrpShaders}"; Flags: exclusive
Name: "shadersnow"; Description: "Warm it &now (about 17 min the first time; the game plays itself, minimised)"; GroupDescription: "{cm:GrpShaders}"; Flags: exclusive unchecked
; The lift and the objects (~0.8 GB of a ~1 GB build, measured on a test install) are only
; needed by a later rebuild; deleting them makes a port update or "Verify and rebuild" a full build. Default keep.
Name: "cleanup"; Description: "&Delete the build's intermediate files afterwards (saves ~0.8 GB; a later update rebuilds from the start)"; GroupDescription: "{cm:GrpBuild}"; Flags: unchecked
Name: "checkupdates"; Description: "&Check for new versions: one anonymous request to GitHub, at most once a day"; GroupDescription: "{cm:GrpOnline}"; Flags: unchecked

[Files]
; For "Check now" on the welcome page, before anything is installed: the bundled Python and version_check.py,
; extracted to {tmp} on the click only. Listed first so they sit at the start of the solid stream (ASSUMPTION: entry
; order = stream order, so the click does not unpack the whole payload); Inno stores the duplicate data once.
Source: "{#Payload}\toolchain\python\*"; DestDir: "{tmp}\vcheck\python"; Flags: dontcopy recursesubdirs
Source: "{#Payload}\kz3-setup\release\version_check.py"; DestDir: "{tmp}\vcheck"; Flags: dontcopy
; run.cmd is the portable zip's entry point (firstrun.py --interactive); the installed copy uses the launcher
Source: "{#Payload}\*"; DestDir: "{app}"; Excludes: "run.cmd"; Flags: ignoreversion recursesubdirs createallsubdirs
; Apps & features icon (UninstallDisplayIcon) and the Start menu's Uninstall entry
Source: "art\kz3-uninstall.ico"; DestDir: "{app}"; Flags: ignoreversion
; Headings font: a temporary file for setup's own process only (AddFontResourceEx FR_PRIVATE in InitializeSetup),
; never installed on the system. The payload itself carries fonts\Rajdhani-Medium.ttf and -Bold.ttf (the game's error
; notice; firstrun.py and the launcher copy them next to build\kz3.exe) and licenses\Rajdhani-OFL.txt
; (assemble_payload.py FONTS, hash-checked against art\art.json like this one).
; The payload also carries runtime\SDL3.dll (zlib; licenses\SDL3-LICENSE.txt): installed by the line above with the rest
; of {app}, copied next to build\kz3.exe by firstrun.py and the launcher (place_sdl3); the game loads it for native pads.
Source: "{#FontDir}\Rajdhani-Bold.ttf"; Flags: dontcopy
; Advanced: "Copy the game files into the install" (opt-in, ~33 GB) with Inno's own progress, so the
; originals may go. The update keeps RPCS3's dev_hdd0\game\<disc title id> layout (BCUS98234, or BCES01007 for the EU disc) (launch.rpcs3_hdd0). Never removed
; by the uninstaller on its own: it asks (CurUninstallStepChanged), the copy may be the player's only one.
Source: "{code:DumpSrc}\*"; DestDir: "{app}\game\disc"; Flags: external recursesubdirs createallsubdirs ignoreversion uninsneveruninstall; Check: CopyGameData
Source: "{code:UpdateSrc}\*"; DestDir: "{app}\game\dev_hdd0\game\{code:DiscTitleId}"; Flags: external recursesubdirs createallsubdirs ignoreversion uninsneveruninstall; Check: CopyGameData
Source: "{code:EbootSrc}"; DestDir: "{app}\game"; DestName: "EBOOT.elf"; Flags: external ignoreversion uninsneveruninstall; Check: CopyEbootFile

[InstallDelete]
; Install over an earlier version: the payload's own folders are replaced whole, so a file a newer version
; dropped cannot linger and be compiled or run. Inno keeps each file's own time stamp, so ninja recompiles only the
; sources that changed. Never listed here: build (the lift and objects; firstrun.py's markers decide what re-runs),
; vfs (the link to the dump), logs, gamedata, game (the copied game files), keybinds.ini, cache, kz3-install.ini.
Type: filesandordirs; Name: "{app}\kz3-setup"
Type: filesandordirs; Name: "{app}\port"
Type: filesandordirs; Name: "{app}\ps3recomp"
Type: filesandordirs; Name: "{app}\re"
Type: filesandordirs; Name: "{app}\cmake"
Type: filesandordirs; Name: "{app}\toolchain"
#if OldDataDirName != ""
; Install over an install of the earlier name: its launcher and shortcuts go, the new ones are made under the new
; name; the install folder itself stays where it was (AppId unchanged, Inno reuses the previous folder).
Type: files; Name: "{app}\{#OldDataDirName}.exe"
Type: files; Name: "{autodesktop}\{#OldDataDirName}.lnk"
Type: filesandordirs; Name: "{autoprograms}\{#OldDataDirName}"
#endif

[Dirs]
Name: "{app}\logs"

[INI]
; Read by the launcher (planned). Paths go to build\paths.json, written by firstrun.py.
Filename: "{app}\kz3-install.ini"; Section: "install"; Key: "portable"; String: "1"; Tasks: portable
Filename: "{app}\kz3-install.ini"; Section: "install"; Key: "portable"; String: "0"; Tasks: not portable
Filename: "{app}\kz3-install.ini"; Section: "online"; Key: "check_updates"; String: "1"; Tasks: checkupdates
Filename: "{app}\kz3-install.ini"; Section: "online"; Key: "check_updates"; String: "0"; Tasks: not checkupdates
; firstrun.py --shaders auto reads it; also after a launcher rebuild
Filename: "{app}\kz3-install.ini"; Section: "shaders"; Key: "prepare"; String: "1"; Tasks: shadersnow
Filename: "{app}\kz3-install.ini"; Section: "shaders"; Key: "prepare"; String: "0"; Tasks: not shadersnow
; Crash reports (Advanced page, opt-in): the switch (report=1 only when ticked)
Filename: "{app}\kz3-install.ini"; Section: "crash"; Key: "report"; String: "1"; Check: CrashOn
Filename: "{app}\kz3-install.ini"; Section: "crash"; Key: "report"; String: "0"; Check: not CrashOn
; the build's address always (the launcher's "Crash reports on or off" can turn reports on later); empty in a build
; without one, and then nothing can be turned on. report=1 is the only switch that sends.
Filename: "{app}\kz3-install.ini"; Section: "crash"; Key: "dsn"; String: "{#CrashDSN}"

[Icons]
Name: "{group}\{#AppDirName}"; Filename: "{app}\{#LauncherExe}"; WorkingDir: "{app}"
; --repair: launch.py checks the install against manifest.json, re-runs the game-file checks, rebuilds what changed
Name: "{group}\Verify and rebuild"; Filename: "{app}\{#LauncherExe}"; Parameters: "--repair"; WorkingDir: "{app}"
Name: "{group}\Prepare shaders again"; Filename: "{app}\{#LauncherExe}"; Parameters: "--prepare-shaders"; WorkingDir: "{app}"
; The saves folder in Explorer, and the move between the install folder and %APPDATA% (copy, check, remove)
Name: "{group}\Open saves folder"; Filename: "{app}\{#LauncherExe}"; Parameters: "--open-data"; WorkingDir: "{app}"
Name: "{group}\Move saves and settings"; Filename: "{app}\{#LauncherExe}"; Parameters: "--switch-data"; WorkingDir: "{app}"
; Runs the game with LD_PSO_TEMPLATE_OUT in the saves folder; the file holds only hashes and render-state numbers
Name: "{group}\Export pipeline template"; Filename: "{app}\{#LauncherExe}"; Parameters: "--export-template"; WorkingDir: "{app}"
; One zip of logs and settings (user names removed) for a bug report; text in tools/release/support_text.py
Name: "{group}\Crash reports on or off"; Filename: "{app}\{#LauncherExe}"; Parameters: "--crash-reports"; WorkingDir: "{app}"
Name: "{group}\Make a support package"; Filename: "{app}\{#LauncherExe}"; Parameters: "--support-package"; WorkingDir: "{app}"
Name: "{group}\Project page"; Filename: "{#SiteURL}"
Name: "{group}\Uninstall {#AppDirName}"; Filename: "{uninstallexe}"; IconFilename: "{app}\kz3-uninstall.ico"
Name: "{autodesktop}\{#AppDirName}"; Filename: "{app}\{#LauncherExe}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#LauncherExe}"; Description: "Start {#AppDirName}"; Flags: nowait postinstall skipifsilent; Check: BuildSucceeded
Filename: "{code:InstallLogPath}"; Description: "Open the install log"; Flags: shellexec postinstall skipifsilent unchecked
Filename: "{#ProjectURL}/issues"; Description: "Report a problem (attach the install log)"; Flags: shellexec postinstall skipifsilent unchecked; Check: BuildBroke
Filename: "{#SiteURL}"; Description: "Open the project page ({#SiteText})"; Flags: shellexec postinstall skipifsilent unchecked

[UninstallDelete]
; The generated derivative goes with the port; the player's dump, update and RPCS3 folder are never touched.
Type: filesandordirs; Name: "{app}\build"
Type: filesandordirs; Name: "{app}\logs"
; The game's writable VFS root (launch.py ensure_vfs): vfs\PS3_GAME is a directory junction to the
; player's dump and is removed as a link by CurUninstallStepChanged(usUninstall) before this list runs; nothing
; here recurses into vfs itself, so the dump is never reached even when the junction could not be removed.
Type: filesandordirs; Name: "{app}\vfs\psarc-cache"
Type: filesandordirs; Name: "{app}\vfs\cache"
Type: files; Name: "{app}\vfs\link.json"
Type: dirifempty; Name: "{app}\vfs"
; Portable layout: the shader cache and the warm-up folder sit in the install folder (saves: AskDeleteSaves).
Type: filesandordirs; Name: "{app}\cache"
Type: files; Name: "{app}\kz3-install.ini"
Type: files; Name: "{app}\kz3-uninstall.ico"
; What the launcher, firstrun.py and shaders.py write into {app}
; and Inno did not install; tools/release/test_uninstall_covers.py keeps this list complete. The first-run and setup
; logs with their time-stamped earlier copies, a support zip written here with --out, the portable marker, and the
; payload's own folders, where Python writes __pycache__ (the embedded toolchain\python as well as kz3-setup).
Type: files; Name: "{app}\{#FileBase}-firstrun*.log"
Type: files; Name: "{app}\{#SetupBase}*.log"
Type: files; Name: "{app}\{#FileBase}-support-*.zip"
Type: files; Name: "{app}\portable.txt"
Type: filesandordirs; Name: "{app}\port"
Type: filesandordirs; Name: "{app}\ps3recomp"
Type: filesandordirs; Name: "{app}\re"
Type: filesandordirs; Name: "{app}\cmake"
Type: filesandordirs; Name: "{app}\toolchain"
; Uninstall run: Python's byte-code caches (kz3-setup\release\__pycache__, written when the
; launcher imports firstrun.py) kept the install folder alive, and the shader caches in the data folder outlived a
; "keep my saves" answer. Caches are not saves: they go either way; gamedata and keybinds.ini stay unless asked.
Type: filesandordirs; Name: "{app}\kz3-setup"
Type: filesandordirs; Name: "{userappdata}\{#AppDirName}\cache"
; Caches live in %LOCALAPPDATA% now (launch.py cache_dir); the folder holds nothing else
Type: filesandordirs; Name: "{localappdata}\{#AppDirName}"
Type: files; Name: "{userappdata}\{#AppDirName}\vkd3d-proton.cache*"
Type: dirifempty; Name: "{userappdata}\{#AppDirName}"

[Code]
var
  DumpPage, UpdatePage: TInputDirWizardPage;
  DumpInfo: TNewStaticText;
  EuNoted: Boolean;
  { A query page with its own Browse button: Inno's dir page refuses an empty field ("You must enter a
    full path"), and this page is optional }
  FontPage: TInputQueryWizardPage;
  TypePage: TInputOptionWizardPage;
  AdvPage: TWizardPage;
  CopyDataChk, CrashChk, FontCopyChk: TNewCheckBox;
  FmaCombo, MemCombo, WinCombo, LangCombo: TNewComboBox;
  EbootPage: TInputFileWizardPage;
  BuildOK, BuildStopped: Boolean;
  StopBtn, FontBrowse: TNewButton;
  StopFile: String;
  OurLog: String;
  CheckBtn, ReleaseBtn: TNewButton;
  CheckLbl: TNewStaticText;
  ReleaseURL: String;
  SilentNoBuild: Boolean;

function LogName: String;
begin
  { <setup name>.log, e.g. Project-Helghast-setup.log next to the started setup.exe }
  Result := ChangeFileExt(ExtractFileName(ExpandConstant('{srcexe}')), '.log');
end;

function InstallLogPath(Param: String): String;
begin
  if OurLog <> '' then Result := OurLog
  else Result := AddBackslash(ExpandConstant('{src}')) + LogName;
end;

function BuildSucceeded: Boolean;
begin
  Result := BuildOK or not WizardIsTaskSelected('buildnow');
end;

function BuildFailed: Boolean;
begin
  Result := not BuildSucceeded;
end;

{ "Report a problem" on the finish page: a failed build, not one the player stopped }
function BuildBroke: Boolean;
begin
  Result := BuildFailed and not BuildStopped;
end;

function Field(Line, Key: String): String;
var
  P: Integer;
begin
  Result := '';
  P := Pos(' ' + Key + '=', Line);
  if P = 0 then Exit;
  Result := Copy(Line, P + Length(Key) + 2, MaxInt);
  P := Pos(' ', Result);
  if P > 0 then Result := Copy(Result, 1, P - 1);
end;

procedure OpenURL(URL: String);
var
  ErrorCode: Integer;
begin
  ShellExec('open', URL, '', '', SW_SHOWNORMAL, ewNoWait, ErrorCode);
end;

procedure ReleaseClick(Sender: TObject);
begin
  { The download happens in the browser, from the release page, on the player's own click. }
  if ReleaseURL <> '' then OpenURL(ReleaseURL);
end;

procedure CheckNowClick(Sender: TObject);
var
  Py, Script, Res, L, Status: String;
  Lines: TArrayOfString;
  RC: Integer;
begin
  { One click = consent for one check. Nothing is sent but the request (version_check.py docstring). }
  CheckBtn.Enabled := False;
  CheckLbl.Caption := 'Asking GitHub...';
  WizardForm.Refresh;
  ExtractTemporaryFiles('{tmp}\vcheck\*');
  { ASSUMPTION: ExtractTemporaryFiles keeps the entries' relative folders under the temp folder (Inno 6 help); untested. }
  Py := ExpandConstant('{tmp}\vcheck\python\python.exe');
  Script := ExpandConstant('{tmp}\vcheck\version_check.py');
  Res := ExpandConstant('{tmp}\vcheck.result');
  L := '';
  if Exec(Py, '"' + Script + '" --current "{#AppVersion}" --timeout 8 --result-file "' + Res + '"', '',
          SW_HIDE, ewWaitUntilTerminated, RC) then
    if LoadStringsFromFile(Res, Lines) and (GetArrayLength(Lines) >= 1) then L := Lines[0];
  { The one log line (merged into the install log next to setup.exe); offline is not an error dialog. }
  if L = '' then L := 'VERSION status=error (the check could not run)';
  Log('version check: ' + L);
  Status := Field(L, 'status');
  if Status = 'newer' then begin
    ReleaseURL := Field(L, 'url');
    if (ReleaseURL = '') or (ReleaseURL = '-') then ReleaseURL := '{#ProjectURL}/releases';
    CheckLbl.Caption := 'Version ' + Field(L, 'latest') + ' is out (this setup is {#AppVersion}).';
    ReleaseBtn.Visible := True;
  end else if Status = 'current' then
    CheckLbl.Caption := 'This is the latest version ({#AppVersion}).'
  else
    CheckLbl.Caption := 'GitHub did not answer (offline or blocked). Nothing else changes.';
  CheckBtn.Enabled := True;
end;

procedure ShowBuildError;
var
  R: Integer;
begin
  { Shown while the build page is still up (its header, not "Installing", behind it; UI review 11). Three labels
    for Yes/No/Cancel: the third is "Close" (wizard test 2026-10-12). }
  repeat
    R := TaskDialogMsgBox('The build did not finish.',
      { A path has no spaces to wrap at: it ran past the dialog's edge (wizard test 2026-10-12); "Open log" opens it }
      'The log is at:' + #13#10 + MinimizePathName(InstallLogPath(''), WizardForm.Font, ScaleX(380)) + #13#10 + #13#10 +
      'Start {#AppName} again: it resumes where it stopped. To report it, attach this log on the issues ' +
      'page; it holds no personal folders and nothing of the game.',
      mbError, MB_YESNOCANCEL, ['Open log', 'Report a problem', 'Close'], 0);
    if R = IDYES then OpenURL(InstallLogPath(''))
    else if R = IDNO then OpenURL('{#ProjectURL}/issues');
  until (R <> IDYES) and (R <> IDNO);
end;

const
  FR_PRIVATE = $10;
  HeadFont = 'Rajdhani';

var
  HeadFontFile: String;

function AddFontResourceEx(FileName: String; Flags: Cardinal; Reserved: Integer): Integer;
  external 'AddFontResourceExW@gdi32.dll stdcall';
function RemoveFontResourceEx(FileName: String; Flags: Cardinal; Reserved: Integer): Integer;
  external 'RemoveFontResourceExW@gdi32.dll stdcall';

{ Rajdhani Bold for the headings only; body text, buttons and pickers keep the Windows UI font
  for readability, DPI, high contrast and screen readers. Loaded for this process only (FR_PRIVATE). }
function InitializeSetup: Boolean;
var
  F: String;
begin
  Result := True;
  try
    ExtractTemporaryFile('Rajdhani-Bold.ttf');
    F := ExpandConstant('{tmp}\Rajdhani-Bold.ttf');
    if AddFontResourceEx(F, FR_PRIVATE, 0) > 0 then HeadFontFile := F;
  except
    HeadFontFile := '';
  end;
  if HeadFontFile <> '' then Log('headings font: Rajdhani loaded (private)')
  else Log('headings font: Rajdhani did not load; the headings keep the Windows UI font');
end;

procedure DeinitializeSetup;
begin
  if HeadFontFile <> '' then RemoveFontResourceEx(HeadFontFile, FR_PRIVATE, 0);
end;

procedure HeadingFont(F: TFont; Size: Integer);
begin
  F.Name := HeadFont;
  F.Style := [fsBold];
  if Size > 0 then F.Size := Size;
end;

procedure ApplyHeadingFonts;
begin
  if HeadFontFile = '' then Exit;
  { Rajdhani is set smaller than Segoe UI at the same point size (lower x-height); a few points more keeps the
    titles' visual size. ASSUMPTION: sizes checked in the Proton screenshots, not on Windows yet. }
  HeadingFont(WizardForm.WelcomeLabel1.Font, 0);
  HeadingFont(WizardForm.FinishedHeadingLabel.Font, 0);
  HeadingFont(WizardForm.PageNameLabel.Font, WizardForm.PageNameLabel.Font.Size + 2);
  { The label keeps the Windows font's height: at +2 pt Rajdhani's descenders (g, y, p) were cut off (wizard test
    2026-10-12). Give it room above and below; the description moves down a little. }
  WizardForm.PageNameLabel.Top := WizardForm.PageNameLabel.Top - ScaleY(2);
  WizardForm.PageNameLabel.Height := WizardForm.PageNameLabel.Height + ScaleY(6);
  WizardForm.PageDescriptionLabel.Top := WizardForm.PageDescriptionLabel.Top + ScaleY(2);
end;

{ Quick checks only; firstrun_check.py (D1-D3, U1-U3, E1) runs as the build's first step and repeats them. }
function EndsWithName(Dir, Name: String): Boolean;
begin
  Result := CompareText(ExtractFileName(RemoveBackslashUnlessRoot(Dir)), Name) = 0;
end;

{ A pick of PS3_GAME or PS3_GAME\USRDIR (one or two levels too deep) becomes the folder that holds PS3_GAME }
function ResolveDump(Dir: String): String;
var
  D: String;
begin
  D := RemoveBackslashUnlessRoot(Trim(Dir));
  Result := D;
  if (D = '') or FileExists(AddBackslash(D) + 'PS3_GAME\PARAM.SFO') then Exit;
  if EndsWithName(D, 'USRDIR') and EndsWithName(ExtractFileDir(D), 'PS3_GAME') then D := ExtractFileDir(D);
  if EndsWithName(D, 'PS3_GAME') then begin
    D := RemoveBackslashUnlessRoot(ExtractFileDir(D));
    if FileExists(AddBackslash(D) + 'PS3_GAME\PARAM.SFO') then Result := D;
  end;
end;

{ A pick of USRDIR inside the update folder becomes the update folder }
function ResolveUpdate(Dir: String): String;
var
  D: String;
begin
  D := RemoveBackslashUnlessRoot(Trim(Dir));
  Result := D;
  if (D <> '') and EndsWithName(D, 'USRDIR') and FileExists(AddBackslash(ExtractFileDir(D)) + 'PARAM.SFO') then
    Result := RemoveBackslashUnlessRoot(ExtractFileDir(D));
end;

{ Little-endian integer of Bytes bytes at the 0-based offset P of S; 0 past the end (the same reader as
  firstrun_check.py read_sfo) }
function SfoInt(S: AnsiString; P, Bytes: Integer): Integer;
var
  B: Integer;
begin
  Result := 0;
  if (P < 0) or (P + Bytes > Length(S)) then Exit;
  for B := Bytes - 1 downto 0 do Result := Result * 256 + Ord(S[P + B + 1]);
end;

{ A NUL-terminated string at the 0-based offset P of S, at most Max bytes }
function SfoText(S: AnsiString; P, Max: Integer): String;
var
  I: Integer;
begin
  Result := '';
  I := P + 1;
  while (I <= Length(S)) and (I <= P + Max) and (Ord(S[I]) <> 0) do begin
    Result := Result + Chr(Ord(S[I]));
    I := I + 1;
  end;
end;

{ One field of a PARAM.SFO (magic NUL P S F, key table start at 8, data table start at 12, entry count at 16, 16-byte
  entries from 20: key offset u16, format u16, length u32, max length u32, data offset u32); '' when absent }
function SfoField(Path, Key: String): String;
var
  S: AnsiString;
  KT, DT, N, I, E: Integer;
begin
  Result := '';
  if not LoadStringFromFile(Path, S) then Exit;
  if (Length(S) < 20) or (Ord(S[1]) <> 0) or (Ord(S[2]) <> 80) or (Ord(S[3]) <> 83) or (Ord(S[4]) <> 70) then Exit;
  KT := SfoInt(S, 8, 4);
  DT := SfoInt(S, 12, 4);
  N := SfoInt(S, 16, 4);
  if N > 200 then N := 200;
  for I := 0 to N - 1 do begin
    E := 20 + 16 * I;
    if SfoText(S, KT + SfoInt(S, E, 2), 32) = Key then begin
      Result := SfoText(S, DT + SfoInt(S, E + 12, 4), SfoInt(S, E + 4, 4));
      Exit;
    end;
  end;
end;

function RegionOf(Id: String): String;
begin
  if Id = 'BCUS98234' then Result := 'US'
  else if Id = 'BCES01007' then Result := 'EU'
  else Result := '';
end;

function DiscIdOf(Dump: String): String;
begin
  Result := SfoField(AddBackslash(Dump) + 'PS3_GAME\PARAM.SFO', 'TITLE_ID');
end;

{ The title id of the picked disc dump; the update goes under it (dev_hdd0\game\<id>) }
function DiscTitleId(Param: String): String;
begin
  Result := DiscIdOf(DumpPage.Values[0]);
  if RegionOf(Result) = '' then Result := 'BCUS98234';
end;

{ '' when the dump is Killzone 3 (US, or EU as an untested region), else the message; Id = the title id found }
function DumpProblem(Dir: String; var Id: String): String;
begin
  Id := '';
  Result := '';
  if not FileExists(AddBackslash(Dir) + 'PS3_GAME\PARAM.SFO') then
    Result := 'This folder does not look like a Killzone 3 disc dump: PS3_GAME\PARAM.SFO is missing. Select the ' +
      'folder that holds PS3_GAME (picking PS3_GAME itself also works).'
  else begin
    Id := DiscIdOf(Dir);
    if Id = '' then
      Result := 'PS3_GAME\PARAM.SFO in this folder has no readable TITLE_ID, so it is not a game dump.'
    else if RegionOf(Id) = '' then
      Result := 'This is ' + Id + ', not Killzone 3 (US BCUS98234 or EU BCES01007).'
    else if not FileExists(AddBackslash(Dir) + 'PS3_GAME\USRDIR\data_ps3.psarc') then
      Result := 'This is Killzone 3 (' + Id + ') but the dump is incomplete: PS3_GAME\USRDIR\data_ps3.psarc is missing.';
  end;
end;

function DumpOK(Dir: String): Boolean;
var
  Id: String;
begin
  Result := DumpProblem(Dir, Id) = '';
end;

function UpdateOK(Dir: String): Boolean;
begin
  Dir := AddBackslash(Dir);
  Result := FileExists(Dir + 'PARAM.SFO') and FileExists(Dir + 'USRDIR\EBOOT.BIN') and
    FileExists(Dir + 'USRDIR\added_data_ps3.psarc.edat');
end;

{ '' when the update's PARAM.SFO names the disc's TITLE_ID and APP_VER 01.14 (as firstrun_check.py U1), else the message }
function UpdateIdProblem(Dir, DiscId: String): String;
var
  Id, Ver: String;
begin
  Result := '';
  Id := SfoField(AddBackslash(Dir) + 'PARAM.SFO', 'TITLE_ID');
  Ver := SfoField(AddBackslash(Dir) + 'PARAM.SFO', 'APP_VER');
  if Id = '' then
    Result := 'The update''s PARAM.SFO has no readable TITLE_ID.'
  else if (DiscId <> '') and (Id <> DiscId) then
    Result := 'This update is for ' + Id + ', but the disc dump is ' + DiscId + '. Pick the update of the same ' +
      'release (dev_hdd0\game\' + DiscId + ').'
  else if Ver <> '01.14' then
    Result := 'This update is version ' + Ver + '; the port is built on 01.14.';
end;

{ "key": "value" of firstrun.py's build\paths.json (json.dump, ASCII): \\ and \" unescaped; '' when absent or when
  the value has other escapes (\u.... for a non-ASCII path: then that folder is asked for again) }
function JsonField(Text, Key: String): String;
var
  P, I: Integer;
  S, R: String;
begin
  Result := '';
  P := Pos('"' + Key + '": "', Text);
  if P = 0 then Exit;
  S := Copy(Text, P + Length(Key) + 5, MaxInt);
  R := '';
  I := 1;
  while I <= Length(S) do begin
    if S[I] = '"' then begin
      Result := R;
      Exit;
    end;
    if S[I] = '\' then begin
      I := I + 1;
      if I > Length(S) then Exit;
      if not ((S[I] = '\') or (S[I] = '"') or (S[I] = '/')) then Exit;
    end;
    R := R + S[I];
    I := I + 1;
  end;
end;

{ The game folders, first found wins: /DUMP= /UPDATE= /EBOOT= on the command line, the earlier install's recorded
  folders (RegisterPreviousData), its build\paths.json (firstrun.py; older installs recorded
  nothing else: a silent upgrade stopped at the dump page for it) }
procedure LoadGamePaths;
var
  JA: AnsiString;
  J, Src: String;
begin
  DumpPage.Values[0] := ExpandConstant('{param:DUMP|}');
  UpdatePage.Values[0] := ExpandConstant('{param:UPDATE|}');
  EbootPage.Values[0] := ExpandConstant('{param:EBOOT|}');
  FontPage.Values[0] := ExpandConstant('{param:FONTS|}');
  if FontPage.Values[0] = '' then FontPage.Values[0] := GetPreviousData('FontDir', '');
  if (FontPage.Values[0] = '') and (WizardForm.PrevAppDir <> '') and
     LoadStringFromFile(AddBackslash(WizardForm.PrevAppDir) + 'build\paths.json', JA) then
  begin
    FontPage.Values[0] := JsonField(String(JA), 'font_src');   { the folder the copy came from }
    if FontPage.Values[0] = '' then FontPage.Values[0] := JsonField(String(JA), 'font_dir');
  end;
  Src := 'command line';
  if (DumpPage.Values[0] = '') and (UpdatePage.Values[0] = '') and (EbootPage.Values[0] = '') then begin
    DumpPage.Values[0] := GetPreviousData('DumpDir', '');
    UpdatePage.Values[0] := GetPreviousData('UpdateDir', '');
    EbootPage.Values[0] := GetPreviousData('EbootFile', '');
    Src := 'earlier install';
  end;
  if (DumpPage.Values[0] = '') and (UpdatePage.Values[0] = '') and (EbootPage.Values[0] = '') and
     (WizardForm.PrevAppDir <> '') and
     LoadStringFromFile(AddBackslash(WizardForm.PrevAppDir) + 'build\paths.json', JA) then begin
    J := String(JA);
    DumpPage.Values[0] := JsonField(J, 'dump');
    UpdatePage.Values[0] := JsonField(J, 'update');
    EbootPage.Values[0] := JsonField(J, 'eboot');
    Src := 'earlier build (build\paths.json)';
  end;
  if (DumpPage.Values[0] = '') and (UpdatePage.Values[0] = '') and (EbootPage.Values[0] = '') then
    Log('game folders: none given or recorded')
  else
    Log('game folders from the ' + Src);
end;

procedure FontBrowseClick(Sender: TObject);
var
  D: String;
begin
  D := FontPage.Values[0];
  if BrowseForFolder('Pick the folder that holds SCE-PS3-*.TTF:', D, False) then FontPage.Values[0] := D;
end;

function IsAdvanced: Boolean;
begin
  Result := TypePage.SelectedValueIndex = 1;
end;

function NewCheck(Page: TWizardPage; ATop: Integer; ACaption: String): TNewCheckBox;
begin
  Result := TNewCheckBox.Create(Page);
  Result.Parent := Page.Surface;
  Result.Top := ATop;
  Result.Width := Page.SurfaceWidth;
  Result.Height := ScaleY(18);
  Result.Caption := ACaption;
end;

function NewCombo(Page: TWizardPage; ATop: Integer; ACaption: String; Items: array of String): TNewComboBox;
var
  I: Integer;
  L: TNewStaticText;
begin
  L := TNewStaticText.Create(Page);
  L.Parent := Page.Surface;
  L.Top := ATop + ScaleY(3);
  L.Caption := ACaption;
  Result := TNewComboBox.Create(Page);
  Result.Parent := Page.Surface;
  Result.Style := csDropDownList;
  Result.Left := ScaleX(150);
  Result.Top := ATop;
  Result.Width := Page.SurfaceWidth - Result.Left;
  for I := 0 to GetArrayLength(Items) - 1 do Result.Items.Add(Items[I]);
  Result.ItemIndex := 0;
end;

{ Step 9s: the Advanced page. The combo texts start with the value firstrun.py takes (ComboValue). }
procedure CreateAdvancedPage;
var
  Y: Integer;
begin
  AdvPage := CreateCustomPage(FontPage.ID, 'Advanced options', 'Everything here is optional; the defaults suit most PCs.');
  Y := 0;
  CopyDataChk := NewCheck(AdvPage, Y, 'Copy the game files into the install (about 33 GB; then the originals may go)');
  Y := Y + ScaleY(24);
  FontCopyChk := NewCheck(AdvPage, Y, 'Copy the PS3 fonts into the install (else read from their folder)');
  FontCopyChk.Checked := True;
  Y := Y + ScaleY(24);
  CrashChk := NewCheck(AdvPage, Y, 'Send crash reports to the project (stack trace and a scrubbed log, no memory)');
#if CrashDSN == ""
  CrashChk.Enabled := False;
  CrashChk.Caption := 'Send crash reports to the project (not available in this build)';
#endif
  Y := Y + ScaleY(32);
  FmaCombo := NewCombo(AdvPage, Y, 'CPU code (FMA3):', ['auto: test this CPU', 'on: AVX + FMA3 (faster)',
    'off: any 64-bit CPU']);
  Y := Y + ScaleY(28);
  MemCombo := NewCombo(AdvPage, Y, 'Build memory:', ['auto: low-memory at 8 GB of RAM or less',
    'on: Low-memory build (slower)', 'off: all cores']);
  Y := Y + ScaleY(28);
  WinCombo := NewCombo(AdvPage, Y, 'Window size:', ['default (1280x720)', '720p', '900p', '1080p', '1440p', '2160p']);
  Y := Y + ScaleY(28);
  LangCombo := NewCombo(AdvPage, Y, 'Game language:', ['default: the system language', 'en: English',
    'fr: French', 'de: German', 'it: Italian', 'es: Spanish', 'nl: Dutch', 'pt: Portuguese', 'ru: Russian',
    'pl: Polish']);
  Y := Y + ScaleY(28);
  with TNewStaticText.Create(AdvPage) do begin
    Parent := AdvPage.Surface;
    Top := Y;
    AutoSize := False;
    WordWrap := True;
    Width := AdvPage.SurfaceWidth;
    Height := ScaleY(32);
    Caption := 'Crash reports are off unless ticked, and can be turned off later. Shader warm-up and the build ' +
      'cleanup are on the previous page.';
  end;
end;

{ 'on: AVX + FMA3 (faster)' -> 'on'; the default entries (index 0) -> '' }
function ComboValue(C: TNewComboBox): String;
var
  P: Integer;
begin
  Result := '';
  if C.ItemIndex <= 0 then Exit;
  Result := C.Items[C.ItemIndex];
  P := Pos(':', Result);
  if P > 0 then Result := Copy(Result, 1, P - 1);
end;

procedure SelectByValue(C: TNewComboBox; V: String);
var
  I: Integer;
begin
  if V = '' then Exit;
  for I := 0 to C.Items.Count - 1 do
    if (C.Items[I] = V) or (Pos(V + ':', C.Items[I]) = 1) then C.ItemIndex := I;
end;

{ Silent installs and installs over an earlier one: the type and the Advanced choices from the command line }
procedure LoadSetupChoices;
var
  T: String;
begin
  T := Lowercase(ExpandConstant('{param:TYPE|}'));
  if T = '' then T := GetPreviousData('SetupType', '');   { an install over an earlier one: its type }
  if T = 'advanced' then TypePage.SelectedValueIndex := 1 else TypePage.SelectedValueIndex := 0;
  CopyDataChk.Checked := ExpandConstant('{param:COPYDATA|0}') = '1';
  CrashChk.Checked := CrashChk.Enabled and (ExpandConstant('{param:CRASHREPORTS|0}') = '1');
  FontCopyChk.Checked := ExpandConstant('{param:FONTCOPY|1}') <> '0';
  SelectByValue(FmaCombo, Lowercase(ExpandConstant('{param:FMA|}')));
  SelectByValue(MemCombo, Lowercase(ExpandConstant('{param:LOWMEM|}')));
  SelectByValue(WinCombo, ExpandConstant('{param:WINDOW|}'));
  SelectByValue(LangCombo, Lowercase(ExpandConstant('{param:LANG|}')));
end;

function CopyGameData: Boolean;
begin
  Result := IsAdvanced and CopyDataChk.Checked;
end;

function CrashOn: Boolean;
begin
  Result := IsAdvanced and CrashChk.Enabled and CrashChk.Checked;
end;

function DumpSrc(Param: String): String;
begin
  Result := DumpPage.Values[0];
end;

function UpdateSrc(Param: String): String;
begin
  Result := UpdatePage.Values[0];
end;

function EbootSrc(Param: String): String;
begin
  Result := EbootPage.Values[0];
end;

{ The EBOOT is copied on its own only when it is not inside the update folder (the update copy carries it) }
function EbootInUpdate: Boolean;
begin
  Result := Pos(Lowercase(AddBackslash(UpdatePage.Values[0])), Lowercase(EbootPage.Values[0])) = 1;
end;

function CopyEbootFile: Boolean;
begin
  Result := CopyGameData and not EbootInUpdate;
end;

{ The game paths firstrun.py gets: the picked folders, or their copies in the install }
function BuildDump: String;
begin
  if CopyGameData then Result := ExpandConstant('{app}\game\disc') else Result := DumpPage.Values[0];
end;

function BuildUpdate: String;
begin
  if CopyGameData then Result := ExpandConstant('{app}\game\dev_hdd0\game\') + DiscTitleId('')
  else Result := UpdatePage.Values[0];
end;

function BuildEboot: String;
begin
  Result := EbootPage.Values[0];
  if not CopyGameData then Exit;
  if EbootInUpdate then
    Result := AddBackslash(BuildUpdate) + Copy(Result, Length(AddBackslash(UpdatePage.Values[0])) + 1, Length(Result))
  else Result := ExpandConstant('{app}\game\EBOOT.elf');
end;

{ The page names what it found in the picked folder (the title id), so a wrong pick is clear before Next }
procedure DumpEditChange(Sender: TObject);
var
  D, Id: String;
begin
  D := ResolveDump(DumpPage.Values[0]);
  if Trim(D) = '' then DumpInfo.Caption := ''
  else if DumpProblem(D, Id) = '' then begin
    if RegionOf(Id) = 'EU' then DumpInfo.Caption := 'Found: ' + Id + ' (EU disc, untested region)'
    else DumpInfo.Caption := 'Found: ' + Id + ' (US disc)';
  end else if Id <> '' then DumpInfo.Caption := 'Found: ' + Id + '. ' + DumpProblem(D, Id)
  else DumpInfo.Caption := 'No Killzone 3 dump found here: PS3_GAME\PARAM.SFO is missing.';
end;

procedure InitializeWizard;
begin
  ApplyHeadingFonts;
  TypePage := CreateInputOptionPage(wpWelcome, 'Setup type', 'How much do you want to choose?',
    'Recommended asks only for your game files and uses the defaults. Advanced shows every option.', True, False);
  TypePage.Add('&Recommended: your disc dump and the 1.14 update, nothing else');
  TypePage.Add('&Advanced: copy the game files into the install, crash reports, fonts, CPU, window, language, ' +
    'shaders, cleanup');
  DumpPage := CreateInputDirPage(wpSelectTasks, 'Your disc dump',
    'Killzone 3, US disc (BCUS98234; the EU disc BCES01007 is untested), decrypted.',
    'Pick the folder that holds PS3_GAME (picking PS3_GAME itself also works). Setup never changes it, and reads it in place unless you choose to copy ' +
    'it (Advanced).', False, '');
  DumpPage.Add('Disc dump folder (holds PS3_GAME):');
  DumpInfo := TNewStaticText.Create(DumpPage);
  DumpInfo.Parent := DumpPage.Surface;
  DumpInfo.Left := 0;
  DumpInfo.Top := DumpPage.Edits[0].Top + DumpPage.Edits[0].Height + ScaleY(10);
  DumpInfo.AutoSize := False;
  DumpInfo.WordWrap := True;
  DumpInfo.Width := DumpPage.SurfaceWidth;
  DumpInfo.Height := ScaleY(48);
  DumpPage.Edits[0].OnChange := @DumpEditChange;
  UpdatePage := CreateInputDirPage(DumpPage.ID, 'Update 1.14',
    'The port is built on 1.14.',
    'Pick the folder named like your disc (BCUS98234 or BCES01007) that holds the 1.14 update.' + #13#10 +
    'RPCS3: after File > Install Packages, it is in RPCS3''s dev_hdd0\game folder.' + #13#10 +
    'PS3: copy dev_hdd0\game\<title id> from the console.', False, '');
  UpdatePage.Add('1.14 update folder (named like the disc, e.g. BCUS98234):');
  EbootPage := CreateInputFilePage(UpdatePage.ID, 'Decrypted EBOOT',
    'The 1.14 EBOOT, decrypted.',
    'EBOOT.elf, the decrypted form of the update''s USRDIR\EBOOT.BIN, made from your own copy. This installer includes no decryption tools.');
  EbootPage.Add('EBOOT.elf:', 'ELF files (*.elf)|*.elf|All files|*.*', '.elf');
  { The kz3.ini font_dir (KZ3_FONT_DIR, patch 5005); empty = the port's own font choice }
  FontPage := CreateInputQueryPage(EbootPage.ID, 'PS3 fonts (optional)',
    'The PS3''s own fonts for the game''s text.',
    'Your PS3 firmware''s dev_flash\data\font folder, e.g. from an emulator install (RPCS3: dev_flash\data\font ' +
    'in its folder). Optional: leave it empty to skip. Its font files are copied into the install unless you untick ' +
    'that on the next page.');
  FontPage.Add('Font folder (holds SCE-PS3-*.TTF):', False);
  FontPage.Edits[0].Width := FontPage.SurfaceWidth - ScaleX(90);
  FontBrowse := TNewButton.Create(FontPage);
  FontBrowse.Parent := FontPage.Surface;
  FontBrowse.Left := FontPage.SurfaceWidth - ScaleX(80);
  FontBrowse.Top := FontPage.Edits[0].Top - ScaleY(1);
  FontBrowse.Width := ScaleX(80);
  FontBrowse.Height := FontPage.Edits[0].Height + ScaleY(2);
  FontBrowse.Caption := 'B&rowse...';
  FontBrowse.OnClick := @FontBrowseClick;
  CreateAdvancedPage;
  { Install over an earlier version: the folders picked last time }
  LoadGamePaths;
  LoadSetupChoices;

  { Welcome page: "Check now" (one-shot, opt-in by the click) and, after a newer release is found, its page. }
  CheckBtn := TNewButton.Create(WizardForm);
  CheckBtn.Parent := WizardForm.WelcomePage;
  CheckBtn.Left := WizardForm.WelcomeLabel2.Left;
  { Two text lines of room under the button (wizard test 2026-10-12: one line ran past the page's right edge) }
  CheckBtn.Top := WizardForm.WelcomePage.Height - ScaleY(72);
  CheckBtn.Width := ScaleX(160);
  CheckBtn.Height := ScaleY(24);
  CheckBtn.Caption := 'Check for a new version';
  CheckBtn.OnClick := @CheckNowClick;
  ReleaseBtn := TNewButton.Create(WizardForm);
  ReleaseBtn.Parent := WizardForm.WelcomePage;
  ReleaseBtn.Left := CheckBtn.Left + CheckBtn.Width + ScaleX(8);
  ReleaseBtn.Top := CheckBtn.Top;
  ReleaseBtn.Width := ScaleX(140);
  ReleaseBtn.Height := CheckBtn.Height;
  ReleaseBtn.Caption := 'Open release page';
  ReleaseBtn.Visible := False;
  ReleaseBtn.OnClick := @ReleaseClick;
  CheckLbl := TNewStaticText.Create(WizardForm);
  CheckLbl.Parent := WizardForm.WelcomePage;
  CheckLbl.Left := CheckBtn.Left;
  CheckLbl.Top := CheckBtn.Top + CheckBtn.Height + ScaleY(6);
  CheckLbl.AutoSize := False;
  CheckLbl.WordWrap := True;
  CheckLbl.Width := WizardForm.WelcomePage.Width - CheckLbl.Left - ScaleX(16);
  CheckLbl.Height := ScaleY(30);
  CheckLbl.Caption := 'One anonymous request to GitHub, only on your click. Nothing about you or the game is sent.';

  { Directory page: Inno's folder icon left an empty gap under Proton (UI review 21) and the page reads the same
    without it, so the text starts at the icon's left edge on every system. }
  WizardForm.SelectDirBitmapImage.Visible := False;
  WizardForm.SelectDirLabel.Width := WizardForm.SelectDirLabel.Left + WizardForm.SelectDirLabel.Width -
    WizardForm.SelectDirBitmapImage.Left;
  WizardForm.SelectDirLabel.Left := WizardForm.SelectDirBitmapImage.Left;
end;

{ Kept with the uninstall entry for the next install over this one }
procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'DumpDir', DumpPage.Values[0]);
  SetPreviousData(PreviousDataKey, 'UpdateDir', UpdatePage.Values[0]);
  SetPreviousData(PreviousDataKey, 'EbootFile', EbootPage.Values[0]);
  SetPreviousData(PreviousDataKey, 'FontDir', FontPage.Values[0]);
  if IsAdvanced then SetPreviousData(PreviousDataKey, 'SetupType', 'advanced')
  else SetPreviousData(PreviousDataKey, 'SetupType', 'recommended');
end;

{ Ready page: the picked game folders too (UI review 15) }
function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
begin
  if CopyGameData then
    Result := MemoDirInfo + NewLine + NewLine + 'Your game files (copied into the install, about 33 GB):' + NewLine
  else
    Result := MemoDirInfo + NewLine + NewLine + 'Your game files (read in place, never copied):' + NewLine;
  Result := Result +
    Space + 'Disc dump: ' + DumpPage.Values[0] + NewLine +
    Space + '1.14 update: ' + UpdatePage.Values[0] + NewLine +
    Space + 'Decrypted EBOOT: ' + EbootPage.Values[0];
  if IsAdvanced and (FontPage.Values[0] <> '') then begin
    Result := Result + NewLine + Space + 'PS3 fonts: ' + FontPage.Values[0];
    if FontCopyChk.Checked then Result := Result + ' (copied)';
  end;
  if IsAdvanced then begin
    Result := Result + NewLine + NewLine + 'Advanced options:' + NewLine + Space + 'CPU code: ' +
      FmaCombo.Items[FmaCombo.ItemIndex] + NewLine + Space + 'Build memory: ' + MemCombo.Items[MemCombo.ItemIndex] + NewLine + Space + 'Window: ' + WinCombo.Items[WinCombo.ItemIndex] + NewLine +
      Space + 'Language: ' + LangCombo.Items[LangCombo.ItemIndex];
    if CrashOn then Result := Result + NewLine + Space + 'Crash reports: on'
    else Result := Result + NewLine + Space + 'Crash reports: off';
  end;
  if MemoGroupInfo <> '' then Result := Result + NewLine + NewLine + MemoGroupInfo;
  { The group labels' leading line break (UI review 20) comes after Inno's indent: move the indent past it }
  StringChangeEx(MemoTasksInfo, Space + NewLine, NewLine + Space, True);
  if MemoTasksInfo <> '' then Result := Result + NewLine + NewLine + MemoTasksInfo;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and BuildFailed then begin
    if BuildStopped then
      WizardForm.FinishedLabel.Caption := '{#AppName} is installed; the build was stopped. Start {#AppName} ' +
        'to resume it where it stopped.'
    else
      WizardForm.FinishedLabel.Caption := '{#AppName} is installed, but the build did not finish. Start ' +
        '{#AppName} to try again from where it stopped, or report the problem with the install log.';
  end;
  if (CurPageID = EbootPage.ID) and (EbootPage.Values[0] = '') then
    EbootPage.Values[0] := AddBackslash(UpdatePage.Values[0]) + 'USRDIR\EBOOT.elf';
end;

{ Recommended: the update's own decrypted EBOOT when it is there (the EBOOT page shows only when it is not) }
procedure DefaultEboot;
var
  E: String;
begin
  E := AddBackslash(UpdatePage.Values[0]) + 'USRDIR\EBOOT.elf';
  if (not IsAdvanced) and ((EbootPage.Values[0] = '') or not FileExists(EbootPage.Values[0])) and FileExists(E) then
    EbootPage.Values[0] := E;
end;

function FontsOK(Dir: String): Boolean;
var
  FR: TFindRec;
begin
  Result := FindFirst(AddBackslash(Dir) + 'SCE-PS3-*.TTF', FR);
  if Result then FindClose(FR);
end;

{ Install folder rules: uninstall removes the whole install folder, so the last folder name is always ours and an
  existing foreign non-empty folder is refused. The player picks the parent only. }
function DirHasFiles(Dir: String): Boolean;
var
  FR: TFindRec;
begin
  Result := False;
  if not FindFirst(AddBackslash(Dir) + '*', FR) then Exit;
  try
    repeat
      if (FR.Name <> '.') and (FR.Name <> '..') then Result := True;
    until Result or (not FindNext(FR));
  finally
    FindClose(FR);
  end;
end;

function IsProtectedFolder(Dir: String): Boolean;
var
  D, Prof: String;
begin
  D := Lowercase(RemoveBackslashUnlessRoot(Dir));
  Prof := RemoveBackslashUnlessRoot(ExpandConstant('{%USERPROFILE|}'));
  Result := (D = Lowercase(RemoveBackslashUnlessRoot(ExtractFileDrive(Dir)))) or
    (D = Lowercase(RemoveBackslashUnlessRoot(ExpandConstant('{win}')))) or
    (D = Lowercase(RemoveBackslashUnlessRoot(ExpandConstant('{commonpf}')))) or
    (D = Lowercase(RemoveBackslashUnlessRoot(ExpandConstant('{userdocs}')))) or
    (D = Lowercase(RemoveBackslashUnlessRoot(ExpandConstant('{userdesktop}')))) or
    ((Prof <> '') and ((D = Lowercase(Prof)) or (D = Lowercase(Prof + '\Downloads'))));
end;

{ True when Dir is OneDrive's folder or inside it (the OneDrive, OneDriveConsumer and OneDriveCommercial
  environment variables, case-insensitive prefix match on whole path components; the same for silent installs) }
function InOneDrive(Dir: String): Boolean;
var
  D, B: String;
  I: Integer;
  Vars: array of String;
begin
  Result := False;
  D := Lowercase(RemoveBackslashUnlessRoot(Dir));
  SetArrayLength(Vars, 3);
  Vars[0] := 'OneDrive';
  Vars[1] := 'OneDriveConsumer';
  Vars[2] := 'OneDriveCommercial';
  for I := 0 to 2 do begin
    B := Lowercase(RemoveBackslashUnlessRoot(GetEnv(Vars[I])));
    if (B <> '') and ((D = B) or (Pos(B + '\', D) = 1)) then
      Result := True;
  end;
end;

{ '' when Dir (the final install folder) is acceptable, else the reason }
function InstallDirProblem(Dir: String): String;
var
  D: String;
begin
  Result := '';
  D := RemoveBackslashUnlessRoot(Dir);
  if CompareText(ExtractFileName(D), '{#AppDirName}') <> 0 then
    Result := 'The install folder must be called "{#AppDirName}" (it is: ' + D + ').'
  else if InOneDrive(D) then
    Result := 'The folder ' + D + ' is inside OneDrive, which syncs the build''s thousands of files while it runs ' +
      'and locks them. Choose a folder outside OneDrive (the default, %LOCALAPPDATA%\Programs\{#AppDirName}, is fine).'
  else if IsProtectedFolder(D) then
    Result := 'This folder cannot be used as the install folder: ' + D
  else if DirExists(D) and DirHasFiles(D) and (not FileExists(AddBackslash(D) + 'unins000.exe')) then
    Result := 'The folder ' + D + ' already exists and holds files that are not from {#AppDirName}. Uninstalling ' +
      'removes the whole install folder, so choose another location or an empty one.';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  FreeB, TotalB: Int64;
  Problem, D: String;
begin
  Result := True;
  { not in silent mode: a False here would wait for a click nobody gives; PrepareToInstall refuses a bad /DIR= }
  if (CurPageID = wpSelectDir) and (not WizardSilent) then begin
    D := RemoveBackslashUnlessRoot(WizardDirValue);
    if (D <> '') and (CompareText(ExtractFileName(D), '{#AppDirName}') <> 0) then begin
      WizardForm.DirEdit.Text := AddBackslash(D) + '{#AppDirName}';
      Log('install folder: appended "{#AppDirName}" to ' + D);
    end;
    Problem := InstallDirProblem(WizardDirValue);
    if Problem <> '' then begin
      MsgBox(Problem, mbError, MB_OK);
      Result := False;
    end;
  end else if CurPageID = DumpPage.ID then begin
    { The folder that holds PS3_GAME is used from here on, whichever level was picked }
    DumpPage.Values[0] := ResolveDump(DumpPage.Values[0]);
    Problem := DumpProblem(DumpPage.Values[0], D);
    if Problem <> '' then begin
      MsgBox(Problem, mbError, MB_OK);
      Result := False;
    end else begin
      Log('disc dump: ' + DumpPage.Values[0] + ', TITLE_ID ' + D + ' (' + RegionOf(D) + ')');
      if (RegionOf(D) = 'EU') and (not EuNoted) then begin
        EuNoted := True;
        MsgBox('EU disc (BCES01007): untested region. A player''s report shows the same 1.14 EBOOT as the US ' +
          'version, but nobody has played it yet. Setup goes on.', mbInformation, MB_OK);
      end;
    end;
  end else if CurPageID = AdvPage.ID then begin
    { ~33 GB of game files + ~1 GB build + room (ASSUMPTION: the dump is the 31.2 GiB disc archive plus the update) }
    if CopyDataChk.Checked and GetSpaceOnDisk64(ExtractFileDrive(WizardDirValue), FreeB, TotalB) and
       (FreeB < Int64(40) * 1024 * 1024 * 1024) then
      Result := MsgBox('The install folder''s drive has less than 40 GB free; copying the game files needs about ' +
        '35 GB. Copy anyway?', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
  end else if CurPageID = UpdatePage.ID then begin
    UpdatePage.Values[0] := ResolveUpdate(UpdatePage.Values[0]);
    DefaultEboot;
    Problem := '';
    if UpdateOK(UpdatePage.Values[0]) then Problem := UpdateIdProblem(UpdatePage.Values[0], DiscIdOf(DumpPage.Values[0]));
    if Problem <> '' then begin
      MsgBox(Problem, mbError, MB_OK);
      Result := False;
    end else if not UpdateOK(UpdatePage.Values[0]) then begin
      { A missing 1.14 update stops here; Next again = check again. }
      MsgBox('The 1.14 update was not found in this folder (PARAM.SFO, USRDIR\EBOOT.BIN and ' +
        'USRDIR\added_data_ps3.psarc.edat are needed). The port is built on 1.14.' + #13#10 + #13#10 +
        'Use the 1.14 update from your own console, or the one installed in RPCS3 ' +
        '(see RPCS3''s documentation).' + #13#10 + #13#10 +
        'Then select the folder again and press Next to check again.', mbError, MB_OK);
      Result := False;
    end;
  end else if (CurPageID = FontPage.ID) and (FontPage.Values[0] <> '') then begin
    if not FontsOK(FontPage.Values[0]) then begin
      MsgBox('No PS3 fonts (SCE-PS3-*.TTF) in this folder. Pick your firmware''s dev_flash\data\font folder, ' +
        'or leave the field empty to skip.', mbError, MB_OK);
      Result := False;
    end;
  end else if CurPageID = EbootPage.ID then begin
    if not FileExists(EbootPage.Values[0]) then begin
      MsgBox('No decrypted EBOOT here. Pick EBOOT.elf, the decrypted form of the update''s ' +
        'USRDIR\EBOOT.BIN made from your own copy.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

{ Silent installs: the three game pages are never validated by the wizard (an empty one stopped a silent
  install with Inno's "You must enter a full path"); PrepareToInstall checks them instead. }
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := WizardSilent and ((PageID = DumpPage.ID) or (PageID = UpdatePage.ID) or (PageID = EbootPage.ID) or
    (PageID = FontPage.ID));
  { Step 9s, Recommended: only the game folders (and the EBOOT when the update has no USRDIR\EBOOT.elf) }
  if (not Result) and (not IsAdvanced) then
    Result := (PageID = wpSelectTasks) or (PageID = FontPage.ID) or (PageID = AdvPage.ID) or
      ((PageID = EbootPage.ID) and FileExists(EbootPage.Values[0]));
  if PageID = AdvPage.ID then Result := Result or WizardSilent;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Missing: String;
begin
  { Silent installs skip the dir page, so NextButtonClick never runs for /DIR=: the same rules are checked here
    (before any file is written) and a bad folder stops the install with the reason; nothing is appended silently. }
  Result := InstallDirProblem(ExpandConstant('{app}'));
  if Result <> '' then Exit;
  if not WizardSilent then Exit;
  DumpPage.Values[0] := ResolveDump(DumpPage.Values[0]);
  UpdatePage.Values[0] := ResolveUpdate(UpdatePage.Values[0]);
  DefaultEboot;
  if (EbootPage.Values[0] = '') and (UpdatePage.Values[0] <> '') then
    EbootPage.Values[0] := AddBackslash(UpdatePage.Values[0]) + 'USRDIR\EBOOT.elf';
  if (DumpPage.Values[0] = '') and (UpdatePage.Values[0] = '') then begin
    SilentNoBuild := True;
    Log('silent install: no game folders given (/DUMP= /UPDATE= /EBOOT=) or recorded by an earlier install; ' +
      'installing the files only, the first start of {#AppDirName} asks for them and builds');
    Exit;
  end;
  Missing := '';
  if DumpPage.Values[0] = '' then Missing := '/DUMP= (the disc dump folder)'
  else if UpdatePage.Values[0] = '' then Missing := '/UPDATE= (the 1.14 update folder)';
  if Missing <> '' then
    Result := 'Silent install stopped: ' + Missing + ' is missing; give /DUMP= and /UPDATE= (and /EBOOT= when the ' +
      'decrypted EBOOT is not the update''s USRDIR\EBOOT.elf), or neither'
  else if DumpProblem(DumpPage.Values[0], Missing) <> '' then
    Result := 'Silent install stopped: ' + DumpPage.Values[0] + ': ' + DumpProblem(DumpPage.Values[0], Missing)
  else if not UpdateOK(UpdatePage.Values[0]) then
    Result := 'Silent install stopped: ' + UpdatePage.Values[0] + ' is not the 1.14 update (PARAM.SFO, ' +
      'USRDIR\EBOOT.BIN and USRDIR\added_data_ps3.psarc.edat are needed)'
  else if UpdateIdProblem(UpdatePage.Values[0], DiscIdOf(DumpPage.Values[0])) <> '' then
    Result := 'Silent install stopped: ' + UpdatePage.Values[0] + ': ' +
      UpdateIdProblem(UpdatePage.Values[0], DiscIdOf(DumpPage.Values[0]))
  else if not FileExists(EbootPage.Values[0]) then
    Result := 'Silent install stopped: no decrypted EBOOT at ' + EbootPage.Values[0];
  if Result <> '' then begin
    { Exit code 7 (Inno: "Preparing to Install" said no); nothing is installed. One line in the log next to setup.exe. }
    SaveStringToFile(InstallLogPath(''), Result + #13#10, True);
    Log(Result);
  end else
    Log('silent install: game folders checked (dump ' + DiscIdOf(DumpPage.Values[0]) + ', update, EBOOT present)');
end;

{ "key=value" field of a PROGRESS/RESULT line }
function StepLabel(Name: String): String;
begin
  { The labels of firstrun.py's step table }
  if Name = 'check' then Result := 'Checking the game files'
  else if Name = 'load' then Result := 'Loading the EBOOT'
  else if Name = 'inventory' then Result := 'Finding SPU programs'
  else if Name = 'ppu-lift' then Result := 'Lifting PPU code'
  else if Name = 'spu-lift' then Result := 'Lifting SPU code'
  else if Name = 'configure' then Result := 'Configuring the build'
  else if Name = 'compile' then Result := 'Compiling'
  else if Name = 'shaders' then Result := 'Preparing shaders (the game runs minimised)'
  else Result := 'Finishing';
end;

function Clock(Secs: Integer): String;
begin
  Result := Format('%d:%.2d:%.2d', [Secs div 3600, (Secs div 60) mod 60, Secs mod 60]);
end;

procedure StopClick(Sender: TObject);
begin
  { firstrun.py --stop-file: it ends the running tool (the whole process tree), leaves no marker for that step and
    writes RESULT step=cancelled; the next start resumes at that step. }
  if MsgBox('Stop the build now?' + #13#10 + #13#10 + 'The steps already done are kept: the next start of ' +
            '{#AppName} resumes where it stopped.', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) <> IDYES then Exit;
  SaveStringToFile(StopFile, 'stop', False);
  Log('build: stop requested by the player');
  StopBtn.Enabled := False;
  StopBtn.Caption := 'Stopping...';
end;

procedure RunBuild;
var
  Page: TOutputProgressWizardPage;
  Py, Script, Status, Params, L, Name, Pct, FontDirArg: String;
  Lines: TArrayOfString;
  RC, Elapsed, P: Integer;
  Finished: Boolean;
begin
  Py := ExpandConstant('{app}\toolchain\python\python.exe');
  Script := ExpandConstant('{app}\kz3-setup\release\firstrun.py');
  FontDirArg := FontPage.Values[0];
  if FontDirArg = '' then FontDirArg := '-';   { firstrun: "-" = no fonts, not the last build's folder }
  if not IsAdvanced then FontDirArg := 'auto'; { Recommended: RPCS3's dev_flash\data\font beside the update, if any }
  Status := ExpandConstant('{tmp}\firstrun.status');
  StopFile := ExpandConstant('{tmp}\firstrun.stop');
  Params := '"' + Script + '" --root "' + ExpandConstant('{app}') + '"' +
    ' --dump "' + BuildDump + '" --update "' + BuildUpdate + '"' +
    ' --eboot "' + BuildEboot + '"' +
    ' --font-dir "' + FontDirArg + '"' +
    ' --toolchain "' + ExpandConstant('{app}\toolchain\llvm-mingw') + '"' +
    ' --log-dir "' + ExpandConstant('{src}') + '" --log-name "' + LogName + '"' +
    ' --status-file "' + Status + '" --stop-file "' + StopFile + '"';
  if WizardIsTaskSelected('cleanup') then Params := Params + ' --cleanup yes' else Params := Params + ' --cleanup no';
  if IsAdvanced then begin   { the Advanced page's choices }
    if not FontCopyChk.Checked then Params := Params + ' --font-copy no';
    if ComboValue(FmaCombo) <> '' then Params := Params + ' --spu-fma ' + ComboValue(FmaCombo);
    if ComboValue(MemCombo) <> '' then Params := Params + ' --low-memory ' + ComboValue(MemCombo);
    if ComboValue(WinCombo) <> '' then Params := Params + ' --window-size ' + ComboValue(WinCombo);
    if ComboValue(LangCombo) <> '' then Params := Params + ' --lang ' + ComboValue(LangCombo);
  end;
  Log('firstrun: ' + Py + ' ' + Params);
  Page := CreateOutputProgressPage('Building {#AppName}', 'From your own game files: usually 5 to 30 minutes.');
  { The wizard has no Cancel after the files are copied: our own Stop (UI review 10). Buttons and the note sit
    under the progress bar; SetProgress in the loop below keeps them answering clicks. }
  StopBtn := TNewButton.Create(Page);
  StopBtn.Parent := Page.Surface;
  StopBtn.Left := Page.ProgressBar.Left;
  StopBtn.Top := Page.ProgressBar.Top + Page.ProgressBar.Height + ScaleY(16);
  StopBtn.Width := ScaleX(110);
  StopBtn.Height := ScaleY(24);
  StopBtn.Caption := 'Stop the build';
  StopBtn.OnClick := @StopClick;
  with TNewStaticText.Create(Page) do begin
    Parent := Page.Surface;
    Left := StopBtn.Left + StopBtn.Width + ScaleX(12);
    Top := StopBtn.Top + ScaleY(4);
    AutoSize := False;
    WordWrap := True;
    Width := Page.SurfaceWidth - Left;
    Height := ScaleY(30);
    Caption := 'Stopping keeps the finished steps: the next start of {#AppName} resumes the build.';
  end;
  Page.SetProgress(0, 1000);
  Page.Show;
  try
    if not Exec(Py, Params, ExpandConstant('{app}'), SW_HIDE, ewNoWait, RC) then begin
      Log('build: could not start (' + SysErrorMessage(RC) + ')');
      SuppressibleMsgBox('Could not start the build (' + SysErrorMessage(RC) + ').', mbError, MB_OK, IDOK);
      Exit;
    end;
    Finished := False;
    { SetProgress processes the window's messages (the Stop button's click, repaints); Sleep alone would not. }
    while not Finished do begin
      Sleep(250);
      Page.SetProgress(Page.ProgressBar.Position, 1000);
      if LoadStringsFromFile(Status, Lines) and (GetArrayLength(Lines) >= 2) then begin
        if Pos('LOG path=', Lines[0]) = 1 then OurLog := Copy(Lines[0], 10, MaxInt);
        L := Lines[1];
        if Pos('RESULT ', L) = 1 then begin
          Finished := True;
          BuildOK := Field(L, 'ok') = '1';
          BuildStopped := Field(L, 'step') = 'cancelled';
        end else if Pos('PROGRESS ', L) = 1 then begin
          Name := Field(L, 'name');
          Elapsed := StrToIntDef(Field(L, 'elapsed'), 0);
          Pct := Field(L, 'pct');
          P := Pos('.', Pct);
          if P > 0 then Pct := Copy(Pct, 1, P - 1);
          if Name = 'compile' then
            Page.SetText(StepLabel(Name) + ' ' + Field(L, 'done') + ' / ' + Field(L, 'total'),
              'Step ' + Field(L, 'step') + ', elapsed ' + Clock(Elapsed))
          else
            Page.SetText(StepLabel(Name), 'Step ' + Field(L, 'step') + ', elapsed ' + Clock(Elapsed));
          Page.SetProgress(StrToIntDef(Pct, 0) * 10, 1000);
        end;
      end;
    end;
    StopBtn.Visible := False;
    if BuildStopped then
      Page.SetText('Build stopped', 'Start {#AppName} to resume it.')
    else if not BuildOK then begin
      Page.SetText('The build did not finish', 'The install log says why.');
      { Silent: no dialog (it would wait for a click nobody makes); the exit code stays 0, the log says it }
      if WizardSilent then Log('silent install: the build did not finish; the first start of {#AppDirName} resumes it')
      else ShowBuildError;
    end else
      Log('build: finished');
  finally
    Page.Hide;
  end;
end;

procedure MergeInnoLog;
var
  Lines: TArrayOfString;
  I, RC: Integer;
  Home, Py: String;
begin
  { One file per run: Inno's own log appended to firstrun.py's, user folder redacted. }
  if not LoadStringsFromFile(ExpandConstant('{log}'), Lines) then begin
    { This never loaded (no merged section in any setup log of earlier versions): Inno still holds
      its log open for writing and LoadStringsFromFile does not share writes. The bundled Python opens it with
      read+write sharing and appends it the same way. }
    Py := ExpandConstant('{app}\toolchain\python\python.exe');
    if not FileExists(Py) then Exit;
    Exec(Py, '-I -c "import sys,re,os; h=os.environ.get(''USERPROFILE'',''''); ' +
      't=open(sys.argv[1],encoding=''utf-8-sig'',errors=''replace'').read(); ' +
      't=re.sub(re.escape(h),''%USERPROFILE%'',t,flags=re.I) if h else t; ' +
      'open(sys.argv[2],''a'',encoding=''utf-8'',newline=''\r\n'').write(''\n# --- Inno Setup log ---\n''+t)" "' +
      ExpandConstant('{log}') + '" "' + InstallLogPath('') + '"', '', SW_HIDE, ewWaitUntilTerminated, RC);
    Exit;
  end;
  Home := GetEnv('USERPROFILE');
  for I := 0 to GetArrayLength(Lines) - 1 do
    if Home <> '' then StringChangeEx(Lines[I], Home, '%USERPROFILE%', True);
  SaveStringToFile(InstallLogPath(''), #13#10 + '# --- Inno Setup log ---' + #13#10, True);
  SaveStringsToFile(InstallLogPath(''), Lines, True);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('buildnow') then begin
    if SilentNoBuild then Log('silent install: build skipped (no game folders); the first start of {#AppDirName} builds')
    else RunBuild;
  end;
  if CurStep = ssDone then MergeInnoLog;
end;

procedure AskDeleteSaves(Dir: String);
begin
  if DirExists(Dir) and not UninstallSilent then
    if MsgBox('Delete your {#AppName} saves and settings too?' + #13#10 + Dir + #13#10 + #13#10 +
              'No keeps them for a later install.', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
      DelTree(Dir, True, True, True);
end;

{ Step 9s: the game files copied on the Advanced page (uninsneveruninstall): may be the player's only copy }
procedure AskDeleteGameCopy(Dir: String);
begin
  if DirExists(Dir) and not UninstallSilent then
    if MsgBox('Delete the copy of your game files in the install folder too (about 33 GB)?' + #13#10 +
              Dir + #13#10#13#10 + 'If you deleted the originals, this is your only copy. No keeps it.', mbConfirmation,
              MB_YESNO or MB_DEFBUTTON2) = IDYES then
      DelTree(Dir, True, True, True);
end;

procedure AskDeletePortableSaves(App: String);
begin
  if not DirExists(App + '\gamedata') or UninstallSilent then Exit;
  if MsgBox('Delete your {#AppName} saves and settings too?' + #13#10 + App + '\gamedata' + #13#10 + #13#10 +
            'No keeps them for a later install.', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then begin
    DelTree(App + '\gamedata', True, True, True);
    DeleteFile(App + '\keybinds.ini');
    RemoveDir(App);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  { Ask before removing saves, default keep (silent uninstall keeps them). A normal install keeps
    saves in APPDATA, a portable one in the install folder (launch.py data_dir: gamedata + keybinds.ini). }
  if CurUninstallStep = usUninstall then begin
    { The junction to the dump: RemoveDirectory on a junction removes the link only, never the target. }
    RemoveDir(ExpandConstant('{app}\vfs\PS3_GAME'));
    Exit;
  end;
  if CurUninstallStep <> usPostUninstall then Exit;
  AskDeleteSaves(ExpandConstant('{userappdata}\{#AppDirName}'));
#if OldDataDirName != ""
  { The data folder's earlier name (launch.py migrate_data moves it on the first start, so it is left only when the
    game was not started since the upgrade, or when both folders held the same file). }
  AskDeleteSaves(ExpandConstant('{userappdata}\{#OldDataDirName}'));
#endif
  { Portable: launch.py's data folder is the install folder itself (gamedata = cellSaveData, keybinds.ini). }
  AskDeletePortableSaves(ExpandConstant('{app}'));
  AskDeleteGameCopy(ExpandConstant('{app}\game'));
end;

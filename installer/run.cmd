@echo off
rem Project Helghast, portable zip: first run.
rem Asks for your own dump, the 1.14 update and the decrypted EBOOT, checks them, then lifts and compiles the port
rem here with one progress bar. Saves and settings stay in this folder (portable). The log is written next to this
rem file (Project-Helghast-firstrun.log). Nothing is downloaded; the optional version check asks first.
rem The window waits for a key only when it was opened by double-clicking this file or when the run failed; started
rem from a console (or with KZ3_NO_PAUSE set) it returns at once. No parenthesised blocks below: a ")" or "&" in the
rem folder name would end such a block early.
setlocal
cd /d "%~dp0"
set "PY=%~dp0toolchain\python\python.exe"
if not exist "%PY%" goto nopy
"%PY%" "%~dp0kz3-setup\release\firstrun.py" --interactive --portable --root "%~dp0." --log-dir "%~dp0." --log-name Project-Helghast-firstrun.log %*
set "RC=%ERRORLEVEL%"
echo.
goto done
:nopy
echo The bundled Python is missing:
echo "%PY%"
echo Unpack the whole zip again, keeping its folders.
set "RC=2"
:done
if defined KZ3_NO_PAUSE exit /b %RC%
if not "%RC%"=="0" goto wait
set "CL=%cmdcmdline:"=%"
if /i "%CL:run.cmd=%"=="%CL%" exit /b 0
:wait
pause
exit /b %RC%

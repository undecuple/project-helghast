@echo off
rem Project Helghast, portable zip: first run.
rem Asks for your own dump, the 1.14 update and the decrypted EBOOT, checks them, then lifts and compiles the port
rem here with one progress bar. Saves and settings stay in this folder (portable). The log is written next to this
rem file (Project-Helghast-firstrun.log). Nothing is downloaded; the optional version check asks first.
setlocal
cd /d "%~dp0"
set "PY=%~dp0toolchain\python\python.exe"
if not exist "%PY%" (
  echo The bundled Python is missing: %PY%
  echo Unpack the whole zip again, keeping its folders.
  pause
  exit /b 2
)
"%PY%" "%~dp0kz3-setup\release\firstrun.py" --interactive --portable --root "%~dp0." --log-dir "%~dp0." --log-name Project-Helghast-firstrun.log %*
set "RC=%ERRORLEVEL%"
echo.
pause
exit /b %RC%

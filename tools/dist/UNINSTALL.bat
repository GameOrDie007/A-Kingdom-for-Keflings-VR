@echo off
REM A Kingdom for Keflings - remove the PCVR port.
REM
REM Takes the two files back out and puts your screen resolution back.
REM The game is then exactly as it was.

setlocal
REM pushd, not cd: cmd cannot stand in a network folder, and pushd maps one.
pushd "%~dp0"

set "DROP=%~1"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" -Path "%DROP%" -Uninstall
echo.
pause

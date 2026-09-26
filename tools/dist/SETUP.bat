@echo off
REM A Kingdom for Keflings - PCVR setup.
REM
REM Double-click it. It finds the game, copies two files in, and checks them.
REM Or DRAG THE GAME'S FOLDER onto this file if it cannot find it.
REM
REM Nothing here needs the internet, and nothing is sent anywhere.

setlocal
REM pushd, not cd: cmd cannot stand in a network folder, and pushd maps one.
pushd "%~dp0"

set "DROP=%~1"
if not defined DROP goto :run
echo.
echo   Using the folder you dropped.

:run
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" -Path "%DROP%"
echo.
pause

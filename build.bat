@echo off
rem Build the 32-bit opengl32.dll proxy.  Runs from its own directory, so it
rem does not depend on the caller's working directory.
setlocal
cd /d "%~dp0"
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat
if not exist "%VCVARS%" (
  echo ERROR: vcvars32.bat not found at "%VCVARS%"
  exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

python tools\gen_proxy.py src
if errorlevel 1 exit /b 1

if not exist build mkdir build
cl /nologo /O2 /W3 /MT /LD /D_CRT_SECURE_NO_WARNINGS ^
   /I extern\openxr\include ^
   src\proxy.c src\vr.c src\dump.c src\input_emu.c src\fovpatch.c src\pointer.c src\generated_thunks.c ^
   /Fobuild\ /Fdbuild\ ^
   /link /MAP:build\opengl32.map /DEF:src\opengl32.def /OUT:build\opengl32.dll /IMPLIB:build\opengl32.lib ^
   user32.lib kernel32.lib advapi32.lib
rem NOTE: openxr_loader.lib is deliberately NOT linked.  A static import would
rem make the proxy -- and therefore the game -- fail to load outright if the
rem loader DLL were missing.  vr.c LoadLibrary's it and degrades to flat.
if errorlevel 1 exit /b 1
echo.
echo Built build\opengl32.dll
endlocal

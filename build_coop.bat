@echo off
rem Builds Coop.dll (32-bit, Release) and puts the mod into ..\Mods\Coop\
rem (the DLL plus the repository's Mod\ folder). Edit the mod's scripts and
rem configs in Mod\, this copies them over.
rem Needs Visual Studio 2019 or later (or its Build Tools) with the
rem "Desktop development with C++" workload.
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if not defined VSDIR (
  echo Visual Studio with the C++ tools was not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul || exit /b 1
set "PATH=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

if not exist build\Bin32\build.ninja (
  cmake -S . -B build\Bin32 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl || exit /b 1
)
cmake --build build\Bin32 || exit /b 1

if not exist "..\Mods\Coop\Bin32" mkdir "..\Mods\Coop\Bin32"
copy /Y build\Bin32\Coop.dll "..\Mods\Coop\Bin32\Coop.dll" >nul || exit /b 1
copy /Y build\info.xml "..\Mods\Coop\info.xml" >nul
copy /Y logo.jpg "..\Mods\Coop\logo.jpg" >nul
robocopy Mod "..\Mods\Coop" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
copy /Y Installer\uninstall.ps1 "..\Mods\Coop\uninstall.ps1" >nul
rem the launcher: in the mod (the installer's copy) and in the game folder
copy /Y build\Bin32\CrysisCoop.exe "..\Mods\Coop\CrysisCoop.exe" >nul || exit /b 1
copy /Y build\Bin32\CrysisCoop.exe "..\CrysisCoop.exe" >nul || exit /b 1
rem a development build: the launcher must not replace it with a release
echo Development build: CrysisCoop.exe does not update the mod while this file exists.> "..\Mods\Coop\noupdate"
echo BUILD_OK

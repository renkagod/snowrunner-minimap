@echo off
rem Configures and builds with the MSVC x64 toolchain. Pass a SnowRunner Sources\Bin path to deploy:
rem   build.cmd "D:\Games\Epic Games\Games\SnowRunner\en_us\Sources\Bin"
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if defined VSCMD_VER goto build
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -products * -latest -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

:build
cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DSNOWMINIMAP_DEPLOY_DIR=%~1" || exit /b 1
cmake --build "%~dp0build" || exit /b 1

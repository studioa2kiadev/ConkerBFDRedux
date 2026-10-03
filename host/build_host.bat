@echo off
rem Builds our program: C:\dev\ConkerRecompWin\host -> host\build\ConkerBFDReloaded.exe
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;C:\dev\ConkerRecompWin\tools;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
taskkill /f /im ConkerBFDReloaded.exe >nul 2>&1
if not exist C:\dev\ConkerRecompWin\host\build\build.ninja "C:\Program Files\CMake\bin\cmake.exe" -S C:\dev\ConkerRecompWin\host -B C:\dev\ConkerRecompWin\host\build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
"C:\Program Files\CMake\bin\cmake.exe" --build C:\dev\ConkerRecompWin\host\build -- -k 0 || exit /b 1
echo BUILD OK

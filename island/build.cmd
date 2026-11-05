@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
    echo Visual Studio with the C++ x64 tools was not found.
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
set "VERSION=%~2"
if "%VERSION%"=="" set "VERSION=1.0.0-dev"
cmake -S "%~dp0." -B "%~dp0out\build" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% -DISLAND_VERSION=%VERSION% || exit /b 1
cmake --build "%~dp0out\build" || exit /b 1
if defined ISLAND_TEST ctest --test-dir "%~dp0out\build" --output-on-failure || exit /b 1

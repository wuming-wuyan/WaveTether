@echo off
rem Verify the CMake build path: configure, build all three targets and run
rem the self-checks. Release artifacts still come from build.bat; this step
rem exists so the two build systems cannot silently drift apart (compile
rem errors, missing resources, wrong VERSIONINFO).
rem
rem Byte-identical output is NOT asserted: the VS generator links default
rem libraries in a different order, which changes the import table layout
rem and therefore the addresses embedded in .text. Same code, same behavior.
setlocal
cd /d "%~dp0.."

set CMAKE=
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
    if exist "%%i\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set CMAKE=%%i\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
)
if not defined CMAKE (
    echo   [FAIL] cmake not found under Visual Studio
    exit /b 1
)

"%CMAKE%" -S . -B build-cmake -G "Visual Studio 17 2022" -A x64 >nul
if errorlevel 1 (
    echo   [FAIL] cmake configure
    exit /b 1
)
"%CMAKE%" --build build-cmake --config Release --parallel >nul
if errorlevel 1 (
    echo   [FAIL] cmake build
    exit /b 1
)

rem portable exe needs tools\ beside it; bt-only and single-file do not.
rem First make sure both build chains report the same version string.
native\WaveTether.exe --version > "%TEMP%\wt_ver_bat.txt" 2>&1
build-cmake\Release\WaveTether.exe --version > "%TEMP%\wt_ver_cmake.txt" 2>&1
fc /b "%TEMP%\wt_ver_bat.txt" "%TEMP%\wt_ver_cmake.txt" >nul
if errorlevel 1 (
    echo   [FAIL] --version differs between build.bat and cmake
    exit /b 1
)
build-cmake\Release\WaveTether-BT-only.exe --selftest >nul
if errorlevel 1 (
    echo   [FAIL] cmake bt-only --selftest
    exit /b 1
)
build-cmake\Release\WaveTether-single.exe --selftest >nul
if errorlevel 1 (
    echo   [FAIL] cmake single-file --selftest
    exit /b 1
)
echo   [OK] cmake build + selftest
exit /b 0

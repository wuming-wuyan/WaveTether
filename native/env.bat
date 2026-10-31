@echo off
rem Shared build environment for build.bat / build_single.bat.
rem ASCII only on purpose: cmd.exe parses .bat in the OEM code page, and
rem non-ASCII bytes can corrupt `rem` / `if` lines on non-UTF8 consoles.
rem
rem Goal: no hard-coded Visual Studio path. If the toolchain is missing,
rem print something a human can act on instead of "cl is not recognized".
rem
rem cmd.exe quirk to respect: a path containing "(x86)" must never appear
rem inside a `for (...) set` or a parenthesised block -- the ")" closes the
rem block early and you get "\Windows was unexpected at this time".
rem Hence: only single-line `if exist` checks below, no blocks around paths.
rem
rem After this script: %VCVARS% is set, vcvars64.bat has been executed, and
rem %WindowsSdkDir% / %WindowsSDKVersion% are available to the caller.

set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" call :TryVswhere
if defined VCVARS goto HaveVcvars

if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS goto NoToolchain

:HaveVcvars
call "%VCVARS%" >nul
if errorlevel 1 goto VcvarsFailed
if not defined WindowsSdkDir goto NoSdk
exit /b 0

:TryVswhere
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call :UseVsPath "%%i"
exit /b 0

:UseVsPath
if not "%~1"=="" if exist "%~1\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%~1\VC\Auxiliary\Build\vcvars64.bat"
exit /b 0

:NoToolchain
echo [ERROR] Visual Studio C++ build tools not found vcvars64.bat.
echo         Install "Visual Studio Build Tools" with "Desktop development with C++".
exit /b 1

:VcvarsFailed
echo [ERROR] vcvars64.bat failed.
exit /b 1

:NoSdk
echo [ERROR] vcvars64.bat did not set WindowsSdkDir; Windows SDK missing?
exit /b 1

@echo off
rem ===========================================================================
rem  WaveTether one-shot check (CI equivalent)
rem
rem  NOTE: keep this file ASCII-only. cmd.exe parses .bat with the OEM code page,
rem  so UTF-8 Chinese here gets mangled and the script breaks. (Project README
rem  documents this pitfall.) Chinese messages live in the .py files instead.
rem
rem  What it guards against:
rem    * code that does not compile
rem    * packaging that misses files (license text not in zip / not embedded)
rem ===========================================================================
setlocal
cd /d "%~dp0.."

set FAILED=0

rem A running instance locks native\WaveTether.exe and the linker reports only
rem "LNK1104: cannot open file" -- which reads like a broken build, not "close the app".
rem Use the absolute findstr path: when this .bat is launched from Git Bash, PATH puts
rem /usr/bin first and a bare "find" resolves to the Unix one ("find: '/I': No such file"),
rem which silently disables this check.
for %%e in (WaveTether.exe WaveTether-BT-only.exe WaveTether-single.exe) do (
    tasklist /FI "IMAGENAME eq %%e" 2>nul | "%SystemRoot%\System32\findstr.exe" /I /C:"%%e" >nul
    if not errorlevel 1 (
        echo [FAIL] %%e is running - close it first ^(it locks the exe we link to^)
        exit /b 1
    )
)

echo.
echo === 1/9 unit tests ===
if not exist native\obj mkdir native\obj >nul 2>&1
call native\env.bat >nul 2>&1
if errorlevel 1 (
    echo   [FAIL] env.bat
    set FAILED=1
) else (
    cl /nologo /O1 /MT /std:c++17 /utf-8 /EHsc /W4 native\test_pure.cpp /Fe:native\obj\test_pure.exe /Fo:native\obj\ >nul 2>&1
    if errorlevel 1 (
        echo   [FAIL] test_pure compile
        set FAILED=1
    ) else (
        native\obj\test_pure.exe
        if errorlevel 1 set FAILED=1
    )
)

echo.
echo === 2/9 build portable ===
call native\build.bat
if errorlevel 1 set FAILED=1

echo.
echo === 3/9 build bluetooth-only ===
call native\build.bat bt
if errorlevel 1 set FAILED=1

echo.
echo === 4/9 build single-file ===
call native\build_single.bat
if errorlevel 1 set FAILED=1

echo.
echo === 5/9 selftest (portable + bluetooth-only + single-file, exit 0 = pass) ===
native\WaveTether.exe --selftest
if errorlevel 1 set FAILED=1
native\WaveTether-BT-only.exe --selftest
if errorlevel 1 set FAILED=1
native\WaveTether-single.exe --selftest
if errorlevel 1 set FAILED=1

echo.
echo === 6/9 static analysis (/analyze, baseline gate) ===
call tools\analyze.bat
if errorlevel 1 set FAILED=1

echo.
echo === 7/9 package ===
if not "%FAILED%"=="0" (
    echo   [SKIP] earlier steps failed - NOT packaging ^(would ship a zip built from stale exes^)
    set FAILED=1
) else (
    python tools\make_dist.py
    if errorlevel 1 set FAILED=1
)

echo.
echo === 8/9 verify artifacts ===
python tools\make_dist.py --verify
if errorlevel 1 set FAILED=1
python tools\check_legal.py
if errorlevel 1 set FAILED=1

echo.
echo === 9/9 cmake build + selftest ===
call tools\cmake_check.bat
if errorlevel 1 set FAILED=1

echo.
if "%FAILED%"=="0" (
    echo ===== ALL PASSED =====
    exit /b 0
)
echo ===== SOME STEPS FAILED ^(see [FAIL] above^) =====
exit /b 1

@echo off
rem ===========================================================================
rem  WaveTether one-shot check (CI equivalent)
rem
rem  NOTE: keep this file ASCII-only. cmd.exe parses .bat with the OEM code page,
rem  so UTF-8 Chinese here gets mangled and the script breaks. (Project README
rem  documents this pitfall.) Chinese messages live in the .py files instead.
rem
rem  What it guards against -- two accidents that actually happened:
rem    * code that does not compile (committed without building)
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
tasklist /FI "IMAGENAME eq WaveTether.exe" 2>nul | "%SystemRoot%\System32\findstr.exe" /I /C:"WaveTether.exe" >nul
if not errorlevel 1 (
    echo [FAIL] WaveTether.exe is running - close it first ^(it locks the exe we link to^)
    exit /b 1
)
tasklist /FI "IMAGENAME eq WaveTether-single.exe" 2>nul | "%SystemRoot%\System32\findstr.exe" /I /C:"WaveTether-single.exe" >nul
if not errorlevel 1 (
    echo [FAIL] WaveTether-single.exe is running - close it first
    exit /b 1
)

echo.
echo === 1/6 unit tests ===
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
echo === 2/6 build portable ===
call native\build.bat
if errorlevel 1 set FAILED=1

echo.
echo === 3/6 build single-file ===
call native\build_single.bat
if errorlevel 1 set FAILED=1

echo.
rem WaveTether.exe covers both portable and bluetooth-only forms; single-file is separate.
echo === 4/6 selftest (portable/bluetooth + single-file, exit 0 = pass) ===
native\WaveTether.exe --selftest
if errorlevel 1 set FAILED=1
native\WaveTether-single.exe --selftest
if errorlevel 1 set FAILED=1

echo.
echo === 5/6 package ===
if not "%FAILED%"=="0" (
    echo   [SKIP] earlier steps failed - NOT packaging ^(would ship a zip built from stale exes^)
    set FAILED=1
) else (
    python tools\make_dist.py
    if errorlevel 1 set FAILED=1
)

echo.
echo === 6/6 verify artifacts ===
python tools\make_dist.py --verify
if errorlevel 1 set FAILED=1
python tools\check_legal.py
if errorlevel 1 set FAILED=1

echo.
if "%FAILED%"=="0" (
    echo ===== ALL PASSED =====
    exit /b 0
)
echo ===== SOME STEPS FAILED ^(see [FAIL] above^) =====
exit /b 1

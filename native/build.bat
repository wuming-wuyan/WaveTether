@echo off
rem Build the portable exe: WaveTether.exe + tools\ folder (small, no temp extraction).
rem Pass "bt" to build the Bluetooth-only variant: same code, only the
rem VERSIONINFO OriginalFilename differs (rc /d BT_ONLY).
setlocal
cd /d "%~dp0"
call env.bat
if errorlevel 1 exit /b 1

set OUTNAME=WaveTether.exe
set RCARGS=
if /I "%~1"=="bt" (
    set OUTNAME=WaveTether-BT-only.exe
    set RCARGS=/d BT_ONLY
)

rc /nologo %RCARGS% /fo app.res app.rc
if errorlevel 1 (
    echo [ERROR] resource compile failed
    exit /b 1
)

cl /nologo /O1 /MT /std:c++17 /utf-8 /EHsc /Brepro /DUNICODE /D_UNICODE ^
   /Gy /Gw /GL ^
   /I"%WindowsSdkDir%Include\%WindowsSDKVersion%cppwinrt" /I"%WindowsSdkDir%Include\%WindowsSDKVersion%um" /I"%WindowsSdkDir%Include\%WindowsSDKVersion%shared" ^
   main.cpp audio.cpp adb.cpp app.res /Fe:%OUTNAME% ^
   /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS windowsapp.lib user32.lib gdi32.lib gdiplus.lib shell32.lib advapi32.lib ole32.lib oleaut32.lib imm32.lib
if errorlevel 1 (
    echo [ERROR] compile failed
    exit /b 1
)

rem Keep intermediate files out of the source directory
if not exist obj mkdir obj
move /y *.obj obj\ >nul 2>&1
if exist app.res move /y app.res obj\ >nul 2>&1
if exist embed.res move /y embed.res obj\ >nul 2>&1
echo [OK] %OUTNAME%
exit /b 0

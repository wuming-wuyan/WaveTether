@echo off
rem Separate build: WaveTether.exe + tools\ folder (small exe, no temp extraction).
setlocal
cd /d "%~dp0"
call env.bat
if errorlevel 1 exit /b 1

rc /nologo /fo app.res app.rc
if errorlevel 1 (
    echo [ERROR] resource compile failed
    exit /b 1
)

cl /nologo /O1 /MT /std:c++17 /utf-8 /EHsc /Brepro /DUNICODE /D_UNICODE ^
   /Gy /Gw /GL ^
   /I"%WindowsSdkDir%Include\%WindowsSDKVersion%cppwinrt" /I"%WindowsSdkDir%Include\%WindowsSDKVersion%um" /I"%WindowsSdkDir%Include\%WindowsSDKVersion%shared" ^
   main.cpp audio.cpp adb.cpp app.res /Fe:WaveTether.exe ^
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
echo [OK] WaveTether.exe
exit /b 0

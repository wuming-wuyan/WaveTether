@echo off
rem Single-file build: one exe with adb/scrcpy embedded (~25MB payload).
rem First run extracts to %TEMP%\WaveTether\rt-<contenthash>\, reused afterwards.
setlocal
cd /d "%~dp0"
call env.bat
if errorlevel 1 (
    echo [ERROR] build environment setup failed
    exit /b 1
)

where python >nul 2>&1
if errorlevel 1 (
    echo [ERROR] python not found, cannot generate embedded resources
    exit /b 1
)

rc /nologo /fo app.res app.rc
if errorlevel 1 (
    echo [ERROR] resource compile failed
    exit /b 1
)

python gen_embed.py
if errorlevel 1 (
    echo [ERROR] gen_embed.py failed
    exit /b 1
)

rc /nologo /fo embed.res embed.rc
if errorlevel 1 (
    echo [ERROR] embedded resource compile failed
    exit /b 1
)

rem Same codegen flags as build.bat (/GL + /LTCG), otherwise the two builds
rem end up with different code sizes for identical source.
cl /nologo /O1 /MT /std:c++17 /utf-8 /EHsc /Brepro /DUNICODE /D_UNICODE ^
   /Gy /Gw /GL ^
   /I"%WindowsSdkDir%Include\%WindowsSDKVersion%cppwinrt" /I"%WindowsSdkDir%Include\%WindowsSDKVersion%um" /I"%WindowsSdkDir%Include\%WindowsSDKVersion%shared" ^
   main.cpp audio.cpp adb.cpp app.res embed.res /Fe:WaveTether-single.exe ^
   /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS windowsapp.lib user32.lib gdi32.lib gdiplus.lib shell32.lib advapi32.lib ole32.lib oleaut32.lib imm32.lib
if errorlevel 1 (
    echo [ERROR] compile failed
    exit /b 1
)

rem Keep intermediate files out of the source directory
if not exist obj mkdir obj
move /y *.obj obj\ >nul 2>&1
rem cmd's MOVE takes one source per invocation -- "move a b dest\" is a syntax error.
if exist app.res move /y app.res obj\ >nul 2>&1
if exist embed.res move /y embed.res obj\ >nul 2>&1
echo [OK] WaveTether-single.exe
exit /b 0

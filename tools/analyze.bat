@echo off
rem Static analysis gate: compile the three TUs with /analyze and compare
rem per-(file, code) warning counts against tools\analyze_baseline.txt.
rem
rem Baseline is "warnings we already know about". An item that grows, or a new
rem item that appears, fails the gate -- swapping one code for another cannot
rem hide a regression behind an unchanged total.
setlocal
cd /d "%~dp0.."
call native\env.bat
if errorlevel 1 exit /b 1

if not exist native\obj\an mkdir native\obj\an >nul 2>&1

pushd native
cl /nologo /c /O1 /MT /std:c++17 /utf-8 /EHsc /W4 /analyze /DUNICODE /D_UNICODE ^
   /I"%WindowsSdkDir%Include\%WindowsSDKVersion%cppwinrt" /I"%WindowsSdkDir%Include\%WindowsSDKVersion%um" /I"%WindowsSdkDir%Include\%WindowsSDKVersion%shared" ^
   main.cpp audio.cpp adb.cpp /Fo:obj\an\ > obj\an.log 2>&1
set CLFAIL=%errorlevel%
popd

if not "%CLFAIL%"=="0" (
    echo [FAIL] analyze compile failed - see native\obj\an.log
    exit /b 1
)

python tools\check_analyze.py native\obj\an.log
exit /b %errorlevel%

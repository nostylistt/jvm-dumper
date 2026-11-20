@echo off
REM Build heap_dumper.dll (x64, Release). Run from a "x64 Native Tools Command Prompt".
setlocal
set OUT=out
if not exist %OUT% mkdir %OUT%

cl.exe /nologo /O2 /MT /W3 /EHsc /std:c++17 ^
    /D_CRT_SECURE_NO_WARNINGS ^
    src\heap_dumper.cpp ^
    /LD ^
    /Fo%OUT%\ /Fe%OUT%\heap_dumper.dll ^
    /link /SUBSYSTEM:WINDOWS kernel32.lib user32.lib

if errorlevel 1 (
    echo.
    echo [FAIL] Build failed.
    exit /b 1
)

echo.
echo [OK] %OUT%\heap_dumper.dll
endlocal

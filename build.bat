@echo off
REM ===================================================================
REM  build.bat  -  在 "x64 Native Tools Command Prompt for VS" 內執行
REM
REM  用法:
REM    build.bat            預設模式（子類化改訊息）
REM    build.bat dr         開啟 USE_DR_HOOK（debug register hook）
REM ===================================================================
setlocal

if not exist obj mkdir obj
if not exist bin mkdir bin

set DRFLAG=
if /I "%1"=="dr" (
    set DRFLAG=/DUSE_DR_HOOK
    echo [build] USE_DR_HOOK enabled
) else (
    echo [build] default mode ^(subclassing^)
)

cl /nologo /EHsc %DRFLAG% /LD /Fo"obj/" /Fe"bin/" hookproc.cpp user32.lib
if errorlevel 1 goto :err

cl /nologo /EHsc /Fo"obj/" /Fe"bin/" installer.cpp user32.lib
if errorlevel 1 goto :err

echo.
echo [build] OK. Output in bin\
goto :eof

:err
echo.
echo [build] FAILED.
exit /b 1

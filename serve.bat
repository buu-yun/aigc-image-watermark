@echo off
setlocal
chcp 65001 >nul
rem ==============================================================
rem  Start the web demo.  Usage:  serve.bat [port]   (default 8080)
rem  Then open http://localhost:8080
rem ==============================================================

if not exist wm_server.exe (
    echo wm_server.exe not found, building...
    call build.bat
    if errorlevel 1 exit /b 1
)

set PORT=%1
if "%PORT%"=="" set PORT=8080

echo.
echo Starting server on port %PORT% ...
echo Close this window (or press Ctrl+C) to stop.
echo.
wm_server.exe %PORT%

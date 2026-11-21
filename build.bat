@echo off
setlocal
rem ================================================================
rem  M1 build script  (C99 / MinGW gcc)
rem  Usage:  build.bat
rem ================================================================
echo [1/3] building wm.exe ...
gcc -std=c99 -O2 -Wall -Wextra -o wm.exe bmp.c lsb.c crc16.c dct.c dctwm.c tilewm.c main.c -lm
if errorlevel 1 (
    echo BUILD FAILED: wm.exe
    exit /b 1
)

echo [2/3] building make_test_image.exe ...
gcc -std=c99 -O2 -Wall -Wextra -o make_test_image.exe make_test_image.c bmp.c
if errorlevel 1 (
    echo BUILD FAILED: make_test_image.exe
    exit /b 1
)

echo [3/3] building wm_server.exe (web demo) ...
gcc -std=c99 -O2 -Wall -Wextra -o wm_server.exe wm_server.c bmp.c lsb.c -lws2_32
if errorlevel 1 (
    echo BUILD FAILED: wm_server.exe
    exit /b 1
)

echo.
echo DONE.  wm.exe  /  make_test_image.exe  /  wm_server.exe
endlocal

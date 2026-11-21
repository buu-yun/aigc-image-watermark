@echo off
setlocal
rem ================================================================
rem  M1 build script  (C99 / MinGW gcc)
rem  Usage:  build.bat
rem ================================================================
echo [1/4] building wm.exe ...
gcc -std=c99 -O2 -Wall -Wextra -o wm.exe bmp.c lsb.c crc16.c dct.c dctwm.c tilewm.c main.c -lm
if errorlevel 1 (
    echo BUILD FAILED: wm.exe
    exit /b 1
)

echo [2/4] building make_test_image.exe ...
gcc -std=c99 -O2 -Wall -Wextra -o make_test_image.exe make_test_image.c bmp.c
if errorlevel 1 (
    echo BUILD FAILED: make_test_image.exe
    exit /b 1
)

echo [3/4] building wm_server.exe (web demo) ...
gcc -std=c99 -O2 -Wall -Wextra -o wm_server.exe wm_server.c bmp.c lsb.c -lws2_32
if errorlevel 1 (
    echo BUILD FAILED: wm_server.exe
    exit /b 1
)

echo [4/4] building libwm.a + wm_demo.exe (M4 library) ...
gcc -std=c99 -O2 -Wall -Wextra -c bmp.c lsb.c crc16.c dct.c dctwm.c tilewm.c crypto.c wm_api.c
if errorlevel 1 (
    echo BUILD FAILED: object files
    exit /b 1
)
ar rcs libwm.a bmp.o lsb.o crc16.o dct.o dctwm.o tilewm.o crypto.o wm_api.o
del /q *.o
gcc -std=c99 -O2 -Wall -Wextra -o wm_demo.exe wm_demo.c -L. -lwm -lm
if errorlevel 1 (
    echo BUILD FAILED: wm_demo.exe
    exit /b 1
)

echo.
echo DONE.  wm.exe / make_test_image.exe / wm_server.exe / libwm.a / wm_demo.exe
endlocal

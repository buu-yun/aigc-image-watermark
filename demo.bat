@echo off
setlocal
chcp 65001 >nul
rem =====================================================================
rem  M1 end-to-end demo:  clean -> build -> cover -> selftest -> embed
rem                       -> extract -> diff -> tamper -> info
rem
rem  Usage:  demo.bat          (double-click also works)
rem  Exit :  0 = all good, 1 = something failed
rem =====================================================================

echo.
echo ============================================================
echo   AI Trusted Watermark Engine - M1 demo
echo ============================================================
echo.

echo [0/7] remove old products (avoids "file locked / permission denied"
echo       when a previous run left files open in an image viewer)
del /f /q test_image.bmp  >nul 2>nul
del /f /q stego.bmp       >nul 2>nul
del /f /q tampered.bmp    >nul 2>nul
del /f /q extracted.txt   >nul 2>nul
del /f /q tampered.txt    >nul 2>nul
del /f /q cover_alt.bmp   >nul 2>nul

echo.
echo [1/7] build
call build.bat
if errorlevel 1 goto :fail

echo.
echo [2/7] generate 512x512 test cover
set COVER=test_image.bmp
make_test_image.exe %COVER% 512 512
if errorlevel 1 (
    echo   [warn] cannot write %COVER%, retry with cover_alt.bmp
    set COVER=cover_alt.bmp
    make_test_image.exe cover_alt.bmp 512 512
    if errorlevel 1 goto :coverfail
)

echo.
echo [3/7] built-in self test (17 assertions)
wm.exe selftest
if errorlevel 1 goto :fail

echo.
echo [4/7] embed UTF-8 Chinese message (324 bytes)
wm.exe embed -i %COVER% -o stego.bmp -f sample_msg_utf8.txt
if errorlevel 1 goto :fail

echo.
echo [5/7] extract and compare with the original byte by byte
wm.exe extract -i stego.bmp -o extracted.txt
if errorlevel 1 goto :fail
fc /b sample_msg_utf8.txt extracted.txt >nul
if errorlevel 1 (echo   [FAIL] extracted bytes differ from the original & goto :fail)
echo   [OK] extracted message is byte-identical to the original

echo.
echo [6/7] invisibility check (max channel diff should be 0 or 1)
wm.exe diff -i %COVER% -j stego.bmp

echo.
echo [7/7] tamper a 60x60 patch, then try to extract again
wm.exe tamper -i stego.bmp -o tampered.bmp -r 100,100,60,60
wm.exe extract -i tampered.bmp -o tampered.txt
if errorlevel 1 (
    echo   [OK] tampering detected: the original text can no longer be recovered
) else (
    fc /b sample_msg_utf8.txt tampered.txt >nul
    if errorlevel 1 (
        echo   [OK] tampering detected: extracted text differs from the original
    ) else (
        echo   [FAIL] tampering was NOT detected
        goto :fail
    )
)

echo.
echo [bonus] info of the watermarked image
wm.exe info -i stego.bmp

echo.
echo ============================================================
echo   DEMO PASSED   (products: stego.bmp / tampered.bmp / extracted.txt)
echo ============================================================
exit /b 0

:coverfail
echo.
echo ============================================================
echo   DEMO FAILED - cannot create a test image.
echo   The folder may be read-only, or the file is locked by
echo   another program (image viewer, WeChat, antivirus).
echo   Try: close such programs, or run this .bat from a folder
echo        you own, e.g. copy the project to D:\watermark
echo ============================================================
exit /b 1

:fail
echo.
echo ============================================================
echo   DEMO FAILED - read the messages above
echo ============================================================
exit /b 1

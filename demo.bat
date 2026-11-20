@echo off
setlocal
chcp 65001 >nul
rem =====================================================================
rem  M1 end-to-end demo:  build -> cover -> selftest -> embed -> extract
rem                       -> diff -> tamper -> extract(fail) -> info
rem
rem  Usage:  demo.bat          (double-click also works)
rem  Exit :  0 = all good, 1 = something failed
rem =====================================================================

echo.
echo ============================================================
echo   AI Trusted Watermark Engine - M1 demo
echo ============================================================
echo.

echo [1/7] build
call build.bat
if errorlevel 1 goto :fail

echo.
echo [2/7] generate 512x512 test cover
make_test_image.exe test_image.bmp 512 512
if errorlevel 1 goto :fail

echo.
echo [3/7] built-in self test (17 assertions)
wm.exe selftest
if errorlevel 1 goto :fail

echo.
echo [4/7] embed UTF-8 Chinese message (324 bytes)
wm.exe embed -i test_image.bmp -o stego.bmp -f sample_msg_utf8.txt
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
wm.exe diff -i test_image.bmp -j stego.bmp

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

:fail
echo.
echo ============================================================
echo   DEMO FAILED - read the messages above
echo ============================================================
exit /b 1

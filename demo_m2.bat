@echo off
setlocal
chcp 65001 >nul
rem =====================================================================
rem  M2 demo:  DCT frequency-domain watermark + CRC + tamper sentinel
rem
rem  Steps: build -> cover(1024x768) -> embed(DCT) -> extract -> verify
rem         -> JPEG q75 -> extract again -> verify -> tamper -> verify
rem
rem  Usage:  demo_m2.bat
rem =====================================================================

echo.
echo ============================================================
echo   M2 demo - DCT watermark survives JPEG
echo ============================================================
echo.

echo [0/7] clean old products
del /f /q cover_m2.bmp stego_m2.bmp got_m2.txt jpeg75.bmp jpeg75.jpg got75.txt t_mid.bmp t_heavy.bmp >nul 2>nul

echo.
echo [1/7] build
call build.bat
if errorlevel 1 goto :fail

echo.
echo [2/7] generate 1024x768 cover (DCT needs more blocks than LSB)
make_test_image.exe cover_m2.bmp 1024 768
if errorlevel 1 goto :fail

echo.
echo [3/7] embed with DCT (-m dct is the default now)
wm.exe embed -i cover_m2.bmp -o stego_m2.bmp -f sample_msg_m2.txt -m dct
if errorlevel 1 goto :fail

echo.
echo [4/7] extract without any compression (should be byte-identical)
wm.exe extract -i stego_m2.bmp -o got_m2.txt -q
if errorlevel 1 goto :fail
fc /b sample_msg_m2.txt got_m2.txt >nul
if errorlevel 1 (echo   [FAIL] extracted bytes differ & goto :fail)
echo   [OK] byte-identical

echo.
echo [5/7] verify the fresh image (expect: [TRUE], exit 0)
wm.exe verify -i stego_m2.bmp -q

echo.
echo [6/7] compress to JPEG q75, decode back, extract again
powershell -NoProfile -ExecutionPolicy Bypass -File tools\jpeg_roundtrip.ps1 -In stego_m2.bmp -Out jpeg75.bmp -Quality 75
wm.exe extract -i jpeg75.bmp -o got75.txt -q
if errorlevel 1 (
    echo   [FAIL] lost the watermark after JPEG
    goto :fail
)
fc /b sample_msg_m2.txt got75.txt >nul
if errorlevel 1 (echo   [FAIL] recovered text differs after JPEG & goto :fail)
echo   [OK] still byte-identical after JPEG q75
echo   verify should now say [SUSPICIOUS]: content intact but pixels re-encoded
wm.exe verify -i jpeg75.bmp -q

echo.
echo [7/7] tamper tests
echo   (a) paint a small patch (1.8%%) -> text still readable, pixels touched => [SUSPICIOUS]
wm.exe tamper -i stego_m2.bmp -o t_mid.bmp -r 300,200,120,120
wm.exe verify -i t_mid.bmp -q
echo   (b) paint a large area over the header -> checksum fails => [FAKE]
wm.exe tamper -i stego_m2.bmp -o t_heavy.bmp -r 10,10,900,650
wm.exe verify -i t_heavy.bmp -q

echo.
echo ============================================================
echo   DONE. products: stego_m2.bmp / jpeg75.bmp / t_mid.bmp / t_heavy.bmp
echo   full regression: powershell -File run_m2_acceptance.ps1
echo ============================================================
exit /b 0

:fail
echo.
echo ============================================================
echo   DEMO FAILED - read the messages above
echo ============================================================
exit /b 1

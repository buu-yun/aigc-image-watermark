@echo off
setlocal
rem ================================================================
rem  Build the browser engine: C -> WebAssembly (needs zig)
rem
rem  Usage: wasm\build_wasm.bat
rem  Needs: zig.exe in PATH, or edit ZIG var below
rem ================================================================

set ZIG=zig
where %ZIG% >nul 2>nul
if errorlevel 1 (
    if exist "%~dp0..\_zig\zig-x86_64-windows-0.17.1\zig.exe" (
        set ZIG=%~dp0..\_zig\zig-x86_64-windows-0.17.1\zig.exe
    ) else (
        echo zig not found. Download from https://ziglang.org/download/
        exit /b 1
    )
)

set ZIG_GLOBAL_CACHE_DIR=%~dp0..\_zigcache
set ZIG_LOCAL_CACHE_DIR=%~dp0..\_zigcache\local

cd /d "%~dp0.."

echo Building wasm\wm.wasm ...
%ZIG% cc -target wasm32-wasi -mexec-model=reactor -O2 -Wall -Wextra -I. ^
  -Dprintf=wm_noop_printf -Dfprintf=wm_noop_fprintf -Wl,--strip-all ^
  -Wl,--export=wm_in_ptr -Wl,--export=wm_out_ptr -Wl,--export=wm_in_cap ^
  -Wl,--export=wm_embed -Wl,--export=wm_extract -Wl,--export=wm_capacity ^
  -o wasm\wm.wasm bmp.c lsb.c wasm\wm_glue.c
if errorlevel 1 exit /b 1

echo.
echo DONE: wasm\wm.wasm
for %%F in (wasm\wm.wasm) do echo Size: %%~zF bytes
echo.
echo Next: powershell -ExecutionPolicy Bypass -File wasm\pack_static.ps1
endlocal

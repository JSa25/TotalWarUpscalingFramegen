@echo off
rem Builds framebridge\helper64.exe, the 64-bit helper of "Upscaling and Framegen for Attila".
rem Needs llvm-mingw (its bin folder on PATH) and NVIDIA's Streamline SDK 2.14.16 headers: download
rem streamline-sdk-v2.14.16.zip from github.com/NVIDIA-RTX/Streamline (release v2.14.16, SHA-256
rem 32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b) and set STREAMLINE to the unpacked folder.
rem NVIDIA's files are not part of this repository.
rem AMD FSR (add-on version 8 on) also needs the AMD FSR SDK 2.4.0 headers: github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK,
rem tag v2.4.0 (commit 7539ae8b); set FFX to its Kits\FidelityFX folder (the one with api, upscalers and framegeneration).
setlocal
cd /d "%~dp0"
if "%STREAMLINE%"=="" (
    echo Set STREAMLINE to the unpacked Streamline SDK 2.14.16 folder first.
    exit /b 1
)
if not exist "%STREAMLINE%\include\sl.h" (
    echo sl.h not found in "%STREAMLINE%\include".
    exit /b 1
)
if "%FFX%"=="" (
    echo Set FFX to the AMD FSR SDK 2.4.0 Kits\FidelityFX folder first.
    exit /b 1
)
if not exist "%FFX%\api\include\ffx_api.h" (
    echo ffx_api.h not found in "%FFX%\api\include".
    exit /b 1
)
set FI=-I"%FFX%\api\include" -I"%FFX%\upscalers\include" -I"%FFX%\framegeneration\include"
if not exist build mkdir build
x86_64-w64-mingw32-clang -O2 -Wall -Wextra -c -o build\helper64.o helper64.c || exit /b 1
x86_64-w64-mingw32-clang++ -O2 -Wall -Wno-unused-parameter -std=c++17 -I"%STREAMLINE%\include" %FI% -c -o build\present12.o present12.cpp || exit /b 1
x86_64-w64-mingw32-clang++ -O2 -Wall -Wno-unused-parameter -std=c++17 -Wno-unknown-pragmas %FI% -c -o build\ffx12.o ffx12.cpp || exit /b 1
x86_64-w64-mingw32-windres -O coff -o build\helper64_res.o helper64.rc || exit /b 1
rem --no-insert-timestamp keeps the build reproducible: the same compiler gives the same file
x86_64-w64-mingw32-clang++ -s -Wl,--no-insert-timestamp -o build\helper64.exe build\helper64.o build\present12.o build\ffx12.o build\helper64_res.o -ld3d11 -ld3d12 -ldxgi -ldxguid -lwintrust -lshell32 || exit /b 1
echo built build\helper64.exe

#!/usr/bin/env bash
# Linux counterpart of build_helper.bat. Use llvm-mingw 20261006 (UCRT).
# STREAMLINE: unpacked SDK v2.14.16, archive SHA256
# 32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b.
# FFX: FidelityFX-SDK v2.4.0 (7539ae8b), Kits/FidelityFX directory.
set -euo pipefail
cd -- "$(dirname -- "$0")"
: "${STREAMLINE:?Set STREAMLINE to Streamline SDK 2.14.16}"
: "${FFX:?Set FFX to FidelityFX SDK 2.4.0 Kits/FidelityFX}"
: "${LLVM_MINGW:?Set LLVM_MINGW to llvm-mingw 20261006 UCRT root}"
cc="$LLVM_MINGW/bin/x86_64-w64-mingw32-clang"
cxx="$LLVM_MINGW/bin/x86_64-w64-mingw32-clang++"
"$cc" --version | head -n 1
[[ -f "$STREAMLINE/include/sl.h" ]]
(cd "$FFX" && sha256sum -c "$OLDPWD/fsr_sdk_headers.sha256")
fi=(-I"$FFX/api/include" -I"$FFX/upscalers/include" -I"$FFX/framegeneration/include")
mkdir -p build
"$cc" -O2 -Wall -Wextra -c -o build/helper64.o helper64.c
"$cxx" -O2 -Wall -Wno-unused-parameter -std=c++17 -I"$STREAMLINE/include" "${fi[@]}" -c -o build/present12.o present12.cpp
"$cxx" -O2 -Wall -Wno-unused-parameter -std=c++17 -Wno-unknown-pragmas "${fi[@]}" -c -o build/ffx12.o ffx12.cpp
"$LLVM_MINGW/bin/x86_64-w64-mingw32-windres" -O coff -o build/helper64_res.o helper64.rc
"$cxx" -s -Wl,--no-insert-timestamp -o build/helper64.exe build/helper64.o build/present12.o build/ffx12.o build/helper64_res.o -ld3d11 -ld3d12 -ldxgi -ldxguid -lwintrust -lshell32
sha256sum build/helper64.exe

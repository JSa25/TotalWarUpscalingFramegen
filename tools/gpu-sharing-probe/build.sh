#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
: "${LLVM_MINGW:?Set LLVM_MINGW to llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64}"
mkdir -p build
for arch in i686 x86_64; do
  "$LLVM_MINGW/bin/$arch-w64-mingw32-clang++" -O2 -Wall -Wextra -std=c++17 -static -municode -Wl,--no-insert-timestamp probe.cpp -o "build/probe-$arch.exe" -ld3d11 -ld3d12 -ldxgi -ldxguid
done

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
: "${STEAM_COMPAT_DATA_PATH:?Set explicitly to the approved target compatdata directory}"
: "${PROTON:?Set PROTON to the installed Proton directory}"
: "${STEAM_RUNTIME:?Set STEAM_RUNTIME to SteamLinuxRuntime_4}"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="${STEAM_COMPAT_CLIENT_INSTALL_PATH:-$HOME/.local/share/Steam}"
export SteamAppId=325610 SteamGameId=325610
export PROTON_USE_WAYLAND=1 MANGOHUD=1
export WINEDLLOVERRIDES='dinput8=n,b;version=n,b;xinput1_3=n,b'
export DXVK_CONFIG='dxgi.maxFrameRate = 160;'
export PROTON_LOG=1 PROTON_LOG_DIR="${PROBE_LOG_DIR:-$PWD/build}"
mkdir -p "$PROTON_LOG_DIR"
# Same Wine session for both architectures: the producer starts the consumer.
exec timeout --signal=TERM --kill-after=10s 180s "$STEAM_RUNTIME/run" -- "$PROTON/proton" run "$PWD/build/probe-i686.exe" "Z:${PWD//\//\\}\\build\\probe-x86_64.exe" "${1:-rgba}" "${2:-3}" "${3:-normal}"

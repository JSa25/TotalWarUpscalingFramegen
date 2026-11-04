# Cross-process GPU sharing probe

This standalone integration test launches a 64-bit D3D12 consumer from a 32-bit
D3D11 producer. It does not load the game, Streamline or FSR, and does not change
the production transport. A successful build is not evidence of Proton sharing
support. Require the complete GPU suite before integrating a transport change.

## Build on Linux

Use the same llvm-mingw release as the helper CI, with the Linux host archive:

```bash
curl -fL https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64.tar.xz -o /tmp/llvm-mingw.tar.xz
printf '%s  %s\n' 5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4 /tmp/llvm-mingw.tar.xz | sha256sum -c -
tar -xf /tmp/llvm-mingw.tar.xz -C /tmp
export LLVM_MINGW=/tmp/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64
bash tools/gpu-sharing-probe/build.sh
```

The checksum is the official GitHub release asset digest. Both executables use
C++17, `-O2 -Wall -Wextra`, and `--no-insert-timestamp`, matching the helper's
conventions. Static C++ runtimes avoid deployment DLLs. This probe needs only
Windows API headers/import libraries; no Streamline/FSR SDK download is needed.
Build outputs and logs under `build/` are ignored.

## Run on the target stack

Exit the game first. Running Proton updates its selected prefix and may write to
the Steam runtime. Obtain the applicable filesystem permission before running
outside the workspace. Do not replace the user's launch options or game files.
Select the same installed Proton/runtime and existing prefix explicitly:

```bash
export PROTON='/mnt/games/SteamLibrary/steamapps/common/Proton 11.0'
export STEAM_RUNTIME='/mnt/games/SteamLibrary/steamapps/common/SteamLinuxRuntime_4'
export STEAM_COMPAT_DATA_PATH='/mnt/games/SteamLibrary/steamapps/compatdata/325610'
# An agent/terminal may not inherit the graphical session's WAYLAND_DISPLAY.
# Export the actual session value if absent; do not guess a socket.
export WAYLAND_DISPLAY=wayland-1 # example: inspect /run/user/$UID/wayland-*
export PROBE_RESULTS=/tmp/twfg-probe-results
bash tools/gpu-sharing-probe/check.sh
```

`run.sh` preserves the recorded Attila variables (`PROTON_USE_WAYLAND=1`,
`MANGOHUD=1`, DLL overrides, and the 160 FPS DXVK config) and adds diagnostic
logging. It leaves Steam's saved launch options alone. It uses app ID 325610 and
the producer starts the consumer in the same Wine session. A fresh `/tmp`
compatdata directory is useful for preliminary diagnostics, but does not establish
acceptance in the player's prefix. `STEAM_COMPAT_CLIENT_INSTALL_PATH` can override
the default `$HOME/.local/share/Steam`.

For one case, use `bash tools/gpu-sharing-probe/run.sh rgba 3 normal`.
The full suite covers both RGBA8 and BGRA8 colour formats, three recreated
resource generations each, plus missing/invalid mapping, invalid duplicated
handle (after earlier successful imports), and injected partial initialization
failure. Negative cases pass only when the consumer reports the specific
expected failure exit (20); unrelated device failures fail the suite.

## Observable acceptance

The producer uses the first hardware DXGI adapter and logs its identity/LUID.
The consumer must resolve that exact LUID; confirm it matches the game's adapter
before accepting results. The production resource flags are retained:

| Slot | Format | D3D11 bindings |
|---|---|---|
| final, hudless | RGBA8 or BGRA8 | SRV |
| depth | R32_FLOAT | SRV, UAV |
| motion | R16G16_FLOAT | SRV, UAV, RT |
| srout0, srout1 | colour | SRV, UAV |
| reactive | R8_UNORM | SRV, UAV, RT |

All textures use DEFAULT usage, one mip/slice/sample, no CPU access, and
SHARED plus SHARED_NTHANDLE. Three D3D11 SHARED fences and seven textures create
named NT handles with the game's access rights. A separate 104-byte test mapping
contains fixed-width handles, PID and adapter LUID; both architectures check its
layout. This test mapping is not a production protocol proposal.

The consumer first logs the named fence opening HRESULT as a baseline, then
uses OpenProcess(PROCESS_DUP_HANDLE), DuplicateHandle and OpenSharedHandle for
every resource. Foreign numeric handles are never passed directly to the import
API. The producer uploads distinct literal bytes (0x11 through 0x77) and signals the game fence on its GPU
context. The consumer GPU waits, copies all seven textures into pitched readback
buffers, writes literal 0x91 and 0xa2 bytes to both output textures, and signals the SR and
presentation fences. The producer waits on both fences, reads both outputs back
and verifies every pixel byte. A handshake keeps the consumer device alive until
return verification, avoiding device teardown completing shared fences early.

Require `PASS probe mode=normal cycles=3`, all texture assertions, matching
adapter identity, and all negative cases. On the target Proton stack, `check.sh`
also requires the named baseline to return `E_NOTIMPL` (`0x80004001`) in all three
normal cycles, reproducing the original failure before accepting the workaround.
The executable remains a general diagnostic: it records the baseline HRESULT
and can succeed on Windows when the named opening succeeds. Such a result, or
any other baseline failure, does not satisfy the Proton suite. Each GPU/IPC wait is bounded
(10 seconds, child readiness up to 40 seconds); the Linux launcher adds a 180
second outer timeout with forced termination after 10 seconds. Every error is
nonzero and includes a resource, operation and error code. Check for leftover
processes after an outer timeout before another run.

RAII closes source/duplicated/process/event/mapping handles and COM objects on
success and partial failures. Per-cycle producer process handle counts reject
growth after the first lazy-driver warmup cycle. Consumer process exit also
provides an OS cleanup boundary; this does not establish production restart
correctness. The separate full game/helper bridge test remains required.

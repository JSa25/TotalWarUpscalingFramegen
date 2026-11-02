# Upscaling and Framegen for Total War

NVIDIA DLSS upscaling (DLAA to Ultra Performance) and DLSS frame generation (2x to 4x) for older Total War games on
GeForce RTX cards, and AMD FSR upscaling and frame generation (2x) on every other card (Attila add-on version 8 on).
This repository holds the source of the 64-bit helper the mods share.

| Game | Mod | Needs |
|---|---|---|
| Total War: ATTILA | Upscaling and Framegen for Attila: [Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3828791891) | [Attila Engine Performance Fix](https://github.com/JSa25/AttilaCPUPerfFix) 6.3 or newer (6.10.0 for AMD FSR) |

Each game's in-game part (the code that hands every frame to this helper, the F11 settings panel, the motion vectors
for units) is part of that game's engine fix and is in its repository; for Attila: `fgbridge.c`, `bridgeprobe.c`,
`fgmotion.c` and `fgpanel.c` in [JSa25/AttilaCPUPerfFix](https://github.com/JSa25/AttilaCPUPerfFix).

Created with the help of AI (Claude by Anthropic). Tested in game by JGdaCaesar.

## How it works

These games are 32-bit Direct3D 11 programs; DLSS only runs in 64-bit programs, and frame generation also needs
Direct3D 12. So the game keeps rendering as it always did, and at a few points per frame the engine fix copies what DLSS
needs into textures shared with this helper: the 3D image before the interface is drawn, its depth, and motion vectors.
The helper runs NVIDIA Streamline (DLSS Super Resolution, DLSS Frame Generation, Reflex) on its own Direct3D 12 device,
hands the upscaled image back to the game (which draws its interface on top), and presents the finished frames,
including the generated ones, in the game's window.

| File | What it does |
|---|---|
| `helper64.c` | The helper process: opens the game's shared memory, textures and fences by name, runs the upscaling thread and the frame generation / present thread |
| `present12.cpp`, `present12.h` | Streamline on Direct3D 12 (and the choice between NVIDIA and AMD FSR per GPU and per the player's pick): initialisation (after checking NVIDIA's signature on `sl.interposer.dll`), DLSS Super Resolution, frame generation, the swapchain in the game's window, NVIDIA's rules for what each GPU may do (frame generation only on RTX 40 / 50, up to 4x only where Streamline reports it), NVIDIA GPUs only |
| `ffx12.cpp`, `ffx12.h`, `camvp.h` | AMD FSR on Direct3D 12 through the AMD FSR SDK's API: upscaling (with the reactive mask the game shares), frame generation with AMD's swapchain in the game's window, and the camera values FSR needs (from the game's view-projection matrix) |
| `fgb_proto.h` | The shared block between the game and the helper (the same file is in the engine fix's repository) |
| `proto.h` | Message layout of an earlier test path that the helper still contains |

## Building

You need:
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (release 20261006, UCRT, x86_64 host), its `bin` folder on `PATH`
- NVIDIA's [Streamline SDK](https://github.com/NVIDIA-RTX/Streamline) **v2.14.16** (`streamline-sdk-v2.14.16.zip`,
  SHA-256 `32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b`), unpacked, and `STREAMLINE` set to its folder
- AMD's [FSR SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) **v2.4.0** (tag `v2.4.0`, commit `7539ae8b`),
  and `FFX` set to its `Kits\FidelityFX` folder (the headers in `api`, `upscalers` and `framegeneration` are used)

Then run `build_helper.bat`. The helper lands in `build\helper64.exe`. The build is reproducible: with the same
llvm-mingw and SDK versions you get exactly the file in the download (compare
`certutil -hashfile build\helper64.exe SHA256` with `helper64.exe.sha256` here and the `SHA256SUMS.txt` in the download).

GitHub builds it too: the [Build workflow](.github/workflows/build.yml) downloads exactly these versions (each pinned by
its SHA-256; the FSR headers are listed in `fsr_sdk_headers.sha256`), runs `build_helper.bat` and fails unless the
helper's SHA-256 equals `helper64.exe.sha256`. A tag `helper-v<N>` publishes a [release](../../releases) with that
`helper64.exe` and its SHA-256; it is the file both mods ship.

### Changing the helper

Attila and Rome 2 ship the same helper, and it comes only from this repository's releases:
1. Change the source here first: merge each game's changes onto this tree (a three-way merge, so the other game's
   comments stay), and put the new build's SHA-256 in `helper64.exe.sha256` in the same commit.
2. Push; the Build workflow must be green. Check its `helper64` artifact on VirusTotal (Microsoft's line must be
   undetected) before tagging.
3. Tag `helper-v<N+1>` and push the tag; the workflow publishes the release.
4. In both mods set `dlss/package/helper_release.txt` to `helper-v<N+1> <SHA-256>`; their `tools/pack_framebridge.sh`
   downloads that release's `helper64.exe` and refuses to pack any other file.

NVIDIA's files (the Streamline and DLSS DLLs) and AMD's (the three signed `amd_fidelityfx_*_dx12.dll`) are not in this
repository; they come with the mod downloads, unmodified, under NVIDIA's licences and the AMD FSR SDK's MIT licence.

## License

See [LICENSE](LICENSE).

This project uses NVIDIA DLSS, NVIDIA Reflex and NVIDIA Streamline. NVIDIA, GeForce RTX, DLSS, Reflex and Streamline are trademarks and/or
registered trademarks of NVIDIA Corporation. This project is not made or endorsed by NVIDIA.

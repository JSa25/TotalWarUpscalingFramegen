# Upscaling and Framegen for Total War

NVIDIA DLSS upscaling (DLAA to Ultra Performance) and DLSS frame generation (2x to 4x) for older Total War games on
GeForce RTX cards. This repository holds the source of the 64-bit helper the mods share. AMD FSR upscaling and frame
generation are being worked on.

| Game | Mod | Needs |
|---|---|---|
| Total War: ATTILA | Upscaling and Framegen for Attila: [Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3828791891) | [Attila Engine Performance Fix](https://github.com/JSa25/AttilaCPUPerfFix) 6.3 or newer |

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
| `present12.cpp`, `present12.h` | Streamline on Direct3D 12: initialisation (after checking NVIDIA's signature on `sl.interposer.dll`), DLSS Super Resolution, frame generation, the swapchain in the game's window, NVIDIA's rules for what each GPU may do (frame generation only on RTX 40 / 50, up to 4x only where Streamline reports it), NVIDIA GPUs only |
| `fgb_proto.h` | The shared block between the game and the helper (the same file is in the engine fix's repository) |
| `proto.h` | Message layout of an earlier test path that the helper still contains |

## Building

You need:
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (release 20261006, UCRT, x86_64 host), its `bin` folder on `PATH`
- NVIDIA's [Streamline SDK](https://github.com/NVIDIA-RTX/Streamline) **v2.14.16** (`streamline-sdk-v2.14.16.zip`,
  SHA-256 `32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b`), unpacked, and `STREAMLINE` set to its folder

Then run `build_helper.bat`. The helper lands in `build\helper64.exe`. The build is reproducible: with the same
llvm-mingw and SDK versions you get exactly the file in the download (compare
`certutil -hashfile build\helper64.exe SHA256` with the `SHA256SUMS.txt` in the download).

NVIDIA's files (the Streamline and DLSS DLLs) are not in this repository; they come with the mod downloads, unmodified,
under NVIDIA's licences.

## License

See [LICENSE](LICENSE).

This project uses NVIDIA DLSS, NVIDIA Reflex and NVIDIA Streamline. NVIDIA, GeForce RTX, DLSS, Reflex and Streamline are trademarks and/or
registered trademarks of NVIDIA Corporation. This project is not made or endorsed by NVIDIA.

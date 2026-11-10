# RESOGUN for PC (resogun-recomp)

A native PC port of **RESOGUN** (Housemarque, PlayStation 4) for Windows and Linux / SteamOS,
built on the [wipeout_pc](https://github.com/AbdelmoumenDeghbouche/wipeout_pc) runtime: the
game's original x86-64 code runs directly on the PC's processor, a runtime replaces the PS4
system libraries, and the graphics are translated to Vulkan by the shadPS4 video core. There is
no CPU emulator.

> **Status: reaches the title screen. Not playable yet.** Every PS4 function RESOGUN imports is
> answered; the game boots, plays its intro video and shows "Press ✕ to start", and on Windows
> (RTX 4070 Ti) it starts loading the game after that. Details: [docs/resogun.md](docs/resogun.md).

No game files are included. You need your own RESOGUN for PS4 (CUSA00038) as fake PKGs.

## Getting a build

Every push to `main` builds both packages (**Actions > Build Windows and Linux**):

- **Windows:** the `resogun-win64` artifact: `resogun.exe` and its DLLs.
- **Linux / Steam Deck:** the `resogun-linux-x86_64` artifact: `resogun` and `lib/` in a .tar.gz.

## First start

1. Unpack the build anywhere and put your PKGs next to `resogun.exe` (Linux: `resogun`): the game
   and, if you have it, the 1.10 update.
2. Run `resogun.exe` (Linux: `./resogun`).
3. The first start unpacks the PKGs into `game/` beside the program (about 1 GB, under a minute,
   with a progress window) and prepares the game once (`out/`). Later starts go straight to the
   game; the PKGs can then be deleted. A ready game folder named `game` (with `eboot.bin`) works
   too.

Settings: the in-game menu (Insert, or R3 + L2), saved in `resogun.ini` beside the program.
Every start writes `user/last_run.log`; attach it to bug reports.

Steam Deck Gaming Mode: add `resogun` as a non-Steam game and choose **Steam Linux Runtime** (not
Proton) under Properties > Compatibility.

## Supported game versions

| Title ID | Region | Version | State |
|---|---|---|---|
| CUSA00038 | US | 01.00 + update 01.10 | Development target |

## Building

Windows: MSYS2 CLANG64 with the packages listed in `.github/workflows/build.yml`, then
`bash build.sh` and `bash packaging/windows/package-resogun.sh` (gives `dist/resogun-win64.zip`).
Linux (Ubuntu 24.04): `bash packaging/linux/ci-deps.sh`, `bash build.sh`,
`bash packaging/linux/package-resogun.sh` (gives `dist/resogun-linux-x86_64.tar.gz`).
`out/gow3-probe` started without arguments behaves as `resogun`. `bash build.sh --test` runs the
unit tests.

## How it works

| Part | Where | What it does |
|---|---|---|
| First start | `src/bootstrap.c` | Finds the game next to the program, unpacks, prepares, starts |
| PKG unpack | `src/boot_pkg.c`, `src/boot_crypto.c` | Decrypts and unpacks fake PKGs (PFS, PFSC); the C version of `scripts/pkg_extract.py` |
| Game profiles | `scripts/game_profiles.py` | System modules to link per title ID |
| Preparation | `src/boot_prepare.c` | Reads `eboot.bin` and links the game's own `libc` and `libSceFios2`; the C version of `scripts/prepare.py`, `link_modules.py`, `function_starts.py`, `content_profile.py` (same output, byte for byte) |
| Import names | `scripts/gen_import_names.py`, `src/import_names.inc` | Maps the game's import NIDs to function names |
| Loader | `src/probe.c` | Loads the image and starts the game's original code |
| Runtime | `src/runtime_*.c` | PS4 system functions |
| Audio3d | `src/runtime_audio.c` | Mixes the bed and the sound objects of the Wwise sink into one output |
| Json2 | `src/runtime_json.c` | sce::Json values, parser and serializer |
| Red zones | `src/redzone.cpp`, `scripts/function_starts.py` | Keeps Windows exception records out of the game's red zones |
| GPU | `gpu/shadps4/` | shadPS4 video core (Vulkan) |

Many internal names (`gow3-probe`, `GOW3_*` variables) still come from the God of War III port
this runtime started as. The Python launcher and scripts of the ports this comes from stay for
development; players get only `resogun`. WipEout Omega Collection and God of War III profiles stay
in place while the runtime is shared.

## Credits

This project is a fork of AbdelmoumenDeghbouche's WipEout Omega Collection port, which is a fork
of FelipePrado0's God of War III port, which comes from Supermedo's Windows port of
deadinside28's original:

- **AbdelmoumenDeghbouche**: [wipeout_pc](https://github.com/AbdelmoumenDeghbouche/wipeout_pc),
  the base of this project (Audio3d/Wwise mixing, Json2, game profiles, red-zone protection).
- **FelipePrado0**: [SpartanPort](https://github.com/FelipePrado0/SpartanPort), the God of War III
  port that runtime comes from.
- **bc160201983**: [medievil_2019_pc_port](https://github.com/bc160201983/medievil_2019_pc_port),
  the first port of another game on that runtime. `pkg_extract.py`, `game_profiles.py` and
  `gen_import_names.py` come from it.
- **deadinside28**: [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc), the original
  native port.
- **Supermedo**: [bloodborne_pc for Windows](https://github.com/Supermedo/bloodborne_pc).
- **cuesta4**: [shadPS4 fork](https://github.com/cuesta4/shadPS4/tree/eltutz) (branch `eltutz`):
  the GNM blend rewrite, depth/stencil state and image aliasing fixes.
- **shadPS4 team**: the [shadPS4](https://github.com/shadps4-emu/shadPS4) video core, the PKG
  format and the symbol names (`tools/aerolib.inl`).

Contributors to the WipEout port: **d4g** (render resolution patches, the Metro Park terrain fix),
**carlosmarind** (save and heap fixes, PS4 Pro mode work), **Extremenis** (the overexposed SDR
fix), **ahmanwoods** (Audio3d IDs), **alhockly** (EU version test), **benhaslett** (game clock
measurements, Linux build fix) and **iannitram** (PS4 FPU state).

Also used: [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [FFmpeg](https://ffmpeg.org),
[SDL3](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui),
[sirit](https://github.com/shadps4-emu/sirit), [magic_enum](https://github.com/Neargye/magic_enum),
[miniz](https://github.com/richgel999/miniz), [xbyak](https://github.com/herumi/xbyak),
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
[FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the AMD FidelityFX SDK,
[MSYS2](https://www.msys2.org), [LLVM](https://llvm.org) and [Pillow](https://python-pillow.org).

## License

GNU GPL v2 or later ([LICENSE](LICENSE)). Third-party components keep their own licenses.

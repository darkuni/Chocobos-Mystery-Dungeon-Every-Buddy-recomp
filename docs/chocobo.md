# Chocobo's Mystery Dungeon EVERY BUDDY! (CUSA14219) - port notes

## The dump

| | |
|---|---|
| PKG | `UP0082-CUSA14219_00-CHOCOBOSDUNGEONE-A0100-V0100.pkg` (fake PKG, category gd) |
| SHA-256 | `4961eb21929b57c2d161584580933f417e8b49e1636f5b7293197dd0e6d485b5` |
| App version | 01.00, no update |
| System version | 5.508 |
| Unpacked | 664 files, 1.5 GB |
| Engine | Unity 2018.2.14f1, IL2CPP (`PS4Player_Il2cpp.self`) |
| Middleware | CRIWARE (`cri_ware_unity.prx`: ADX2 audio, Sofdec video) |

## Dump check (first boot, 2026-11-10)

The first test crashed in libc's start, in the first call into the eboot. The dump is the cause:
in `UP0082-CUSA14219_00-CHOCOBOSDUNGEONE-A0100-V0100.pkg` (SHA-256 above) the code segment of
`eboot.bin` (22,355,376 bytes) and of `Media/Modules/Il2CppUserAssemblies.prx` (22,704,088 bytes)
is all zeros, and `sce_discmap.plt` is 98% zeros. The PKG stores those blocks uncompressed and
every sector is decrypted, so the zeros are in the PKG itself; libc, Fios2, NpToolkit2, PS4Util
and all four plugins are intact. Typical of a dumper that could not decrypt those SELFs.

Both preparers now refuse a module whose code is all zeros, with a message saying the dump is
incomplete, instead of crashing later. A good base dump (or an update PKG, which carries its own
eboot and modules) is needed.

## Layout

```
eboot.bin                         Unity player + libil2cpp (23 MB)
sce_module/                       libc, Fios2, NpToolkit2, JobManager, Face/Hand/Head trackers, ...
Media/Modules/Il2CppUserAssemblies.prx   the game's C# compiled to native code (29 MB)
Media/Modules/PS4Util.prx
Media/Plugins/cri_ware_unity.prx  CRIWARE
Media/Plugins/SaveData.prx, UnityNpToolkit2.prx, RSPlugin.prx
Media/Metadata/global-metadata.dat   IL2CPP metadata
Media/*.assets, level*, *.resS    Unity data; Media/StreamingAssets: CRI files
```

## Linking

Nine modules are linked into one image at prepare time (`game_profiles.py` CUSA14219 and
`bootstrap.c` MODULES, the same list):

| Module | Why |
|---|---|
| libc.prx, libSceFios2.prx | as every port |
| libSceNpToolkit2.prx | needed by UnityNpToolkit2 |
| Media/Modules/PS4Util.prx, Il2CppUserAssemblies.prx | in the eboot's DT_NEEDED; the IL2CPP game code |
| Media/Plugins/cri_ware_unity.prx, SaveData.prx, UnityNpToolkit2.prx, RSPlugin.prx | opened by Unity at run time |

Module names with a `/` are relative to the game folder (`module_path` in link_modules.py,
boot_prepare.c, function_starts.py, gen_import_names.py). Every module has TLS, so the runtime's
TLS module limit went from 7 to 16.

Python and C prepare give byte-identical `boot-linked.bin`, `modules.txt` and `functions.bin`
for this game (checked on the US 01.00 dump). Some plugins have an `.eh_frame_hdr` without a
search table; they now give no function starts instead of failing the prepare.

## Plugins: sceKernelLoadStartModule / sceKernelDlsym (src/dynlib.c)

Unity opens its plugins by path and looks their functions up by C name. All of them are linked
and started before the game (link order, like the PS4 dynamic linker), so:

- `modules.txt` (written by both preparers) lists the linked modules in table order.
- `sceKernelLoadStartModule(path)` matches the base name (path and .prx/.sprx ignored) and hands
  out handle 0x1000 + index; `sceKernelStopUnloadModule` on those is a no-op.
- `sceKernelDlsym(handle, name)` hashes the name to its NID (SHA-1 of name + SDK suffix, first 8
  bytes reversed, base64 with `+` and `-`) and looks it up in the module's exports, read once
  from its PRX with the prepare's parser (`boot_module_exports`).

Checked against the real files: 376 of the 386 CRIWARE names in the game's IL2CPP metadata
resolve. The other 10 are `criFsInstaller_*`, which the PS4 build of the plugin does not have.

## Import names

`src/import_names.inc` maps NID#library to names for the runtime's tables. It only had the
earlier ports' imports: `gen_import_names.py` over all nine modules added 581 names. Without
them most of the game's calls would have stopped at "first unsupported PS4 import" even where the
runtime has the function.

## Imports answered for this game

All 922 imports left to the host runtime have an answer (static check). New in this port:

- **SaveDataMemory2** (`runtime_savedata.c`): slots (`memory.dat`, `memory<slot>.dat`), param and
  icon, sync events; plus the SaveData plugin's GetParam, LoadIcon, GetMountInfo, Backup,
  GetEventResult and progress calls. Layouts from shadPS4.
- **Videodec2** (`runtime_videodec2.c`): H.264/HEVC through FFmpeg into the game's NV12 buffers
  with the picture info after the picture, for CRIWARE's Sofdec movies. After shadPS4.
- **Threads** (`runtime_thread.c`): scePthreadGetschedparam, AttrGetstack/Getstackaddr,
  pthread_detach/equal/setcancelstate, pthread cleanup push/pop.
- **sceKernelDirectMemoryQuery** (`runtime_memory.c`).
- **runtime_unity.c**: POSIX semaphores, inet_pton (IPv4), offline sockets, Unity's exception
  handler calls (noted, not hooked), and the offline answers for PS VR, PS Move, social screen,
  Remote Play, live streaming, add-on content, SSL and PSN title storage.

## Open questions for the first boot

- `sceKernelRaiseException` is not delivered. If IL2CPP's GC uses it to stop threads, the
  first collection will hang; the log line "sceKernelRaiseException ... not delivered" shows it.
- Plugin module_start runs before the game's entry, not when Unity opens the plugin.
- The CRIWARE audio path (Ajm / AudioOut / Audio3d) is the runtime's existing one.

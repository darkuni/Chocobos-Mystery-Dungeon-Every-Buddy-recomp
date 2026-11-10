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

The eboot's DT_NEEDED list names `Il2CppUserAssemblies.prx` and `PS4Util.prx` next to libc, so
both are linked into the image at prepare time like the system modules. Module names with a `/`
are now taken relative to the game folder (`link_modules.py` `module_path`, `boot_prepare.c`
`module_path`).

Linked with `libc.prx, libSceFios2.prx, Media/Modules/PS4Util.prx,
Media/Modules/Il2CppUserAssemblies.prx`: all four load, 283 imports bind between the modules,
647 go to the host runtime. Nothing in the user assemblies imports back into the eboot.

The Media/Plugins PRXs are not in DT_NEEDED: Unity opens them with `sceKernelLoadStartModule` and
looks up their functions with `sceKernelDlsym`. The runtime answers both with "not available"
today (`runtime_extra.c`). This is the first real piece of work: without `cri_ware_unity.prx`
there is no sound and no video.

## Imports the runtime does not answer yet (77 of 647)

Static count from the runtime and shadPS4 sources; the first boot log is the real list.

| Library | Count | Functions |
|---|---|---|
| libSceSsl | 8 | sceSslGetCaCerts, sceSslGetMemoryPoolStats, sceSslGetIssuerName, sceSslGetSerialNumber, sceSslGetSubjectName, sceSslGetNameEntryCount, sceSslGetNameEntryInfo, sceSslFreeSslCertName |
| libkernel | 8 | scePthreadGetschedparam, sceKernelSetGPO, sceKernelDirectMemoryQuery, sceKernelSetVirtualRangeName, __pthread_cleanup_pop_imp, __pthread_cleanup_push_imp, scePthreadAttrGetstack, scePthreadAttrGetstackaddr |
| libScePosix | 8 | pthread_equal, inet_pton, sendmsg, recvmsg, chmod, pthread_detach, pthread_setcancelstate, sem_post |
| libSceSystemService | 7 | sceSystemServiceDisablePersonalEyeToEyeDistanceSetting, sceSystemServiceEnablePersonalEyeToEyeDistanceSetting, sceSystemServiceReportAbnormalTermination, sceSystemServiceShowControllerSettings, sceSystemServiceEnableSuspendConfirmationDialog, sceSystemServicePowerTick, sceSystemServiceDisableSuspendConfirmationDialog |
| libSceSocialScreenDialog | 6 | Initialize, Open, Close, Terminate, GetResult, UpdateStatus |
| libSceVrTracker | 5 | CpuProcess, NotifyEndOfCpuProcess, GetPlayAreaWarningInfo, GpuWait, ResetOrientationRelative |
| libSceAppContent | 5 | AddcontMount, AddcontUnmount, TemporaryDataUnmount, TemporaryDataGetAvailableSpaceKb, DownloadDataGetAvailableSpaceKb |
| libSceMove | 5 | Init, Term, Open, SetLightSphere, ReadStateRecent |
| libSceSocialScreen | 5 | InitializeSeparateModeParameter, SetMode, OpenSeparateMode, CloseSeparateMode, ConfigureSeparateMode |
| libSceSaveData | 4 | sceSaveDataSetupSaveDataMemory2, GetSaveDataMemory2, SetSaveDataMemory2, SyncSaveDataMemory |
| libSceHmd | 3 | ReprojectionSetUserEventEnd, ReprojectionStartMultilayer, ReprojectionClearUserEventStart |
| libSceRemoteplay | 3 | Approve, Prohibit, ProhibitStreaming |
| libSceHttp | 2 | SetRedirectCallback, SetAuthEnabled |
| libScePad | 2 | DeviceClassParseData, DeviceClassGetExtendedInformation |
| libkernel_unity | 2 | sceKernelInstallExceptionHandler, sceKernelRaiseException |
| libSceCamera | 1 | IsValidFrameData |
| libSceVideoOut | 1 | sceVideoOutConfigureOptionsInitialize_ |
| libSceAudioOut | 1 | SetMixLevelPadSpk |
| libSceDiscMap | 1 | fJgP+wqifno |

Most are stubs (no VR, no Move, no social screen, offline). The ones that need real work:

- **SaveDataMemory2**: Unity's PS4 save path keeps the save in a memory block; map it to a file
  in `user/`.
- **libkernel_unity exceptions**: Unity installs a handler for hardware exceptions (IL2CPP null
  checks); this has to reach the host's signal / vectored exception handling.
- **sceKernelDirectMemoryQuery**, pthread cleanup and attr getters: small kernel additions.

## Plan

1. Plugin loading: `sceKernelLoadStartModule` / `sceKernelDlsym` for the Media/Plugins PRXs
   (link them at prepare time too and hand out handles, or load them at run time).
2. The kernel, SaveDataMemory2 and Unity exception functions above; stubs for the rest.
3. First boot on Windows and the Deck from CI builds, then work through the log.

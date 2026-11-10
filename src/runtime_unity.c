/* Answers for Unity's PS4 player and plugins (Chocobo's Mystery Dungeon EVERY BUDDY!) that no
 * earlier port imported: POSIX semaphores and sockets, kernel extras, and the offline answers of
 * devices and services the PC has no counterpart for (PS VR, PS Move, social screen, PSN title
 * storage, Remote Play, live streaming). */
#include "runtime.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ERR(n) ((int32_t)(UINT32_C(0x80020000) | (n)))

static ABI int32_t ok(void) { return 0; }

/* ---- POSIX semaphores: sem_t holds a pointer to the host object ---- */
typedef struct { pthread_mutex_t lock; pthread_cond_t cond; uint32_t value; } Sem;
static void set_errno(int e) { *runtime_errno() = e; }
static ABI int32_t sem_init_guest(void **sem, int shared, uint32_t value) {
    (void)shared;
    if (!sem) { set_errno(22); return -1; }
    Sem *s = calloc(1, sizeof(*s));
    if (!s) { set_errno(12); return -1; }
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->value = value;
    *sem = s;
    return 0;
}
static ABI int32_t sem_destroy_guest(void **sem) {
    if (!sem || !*sem) { set_errno(22); return -1; }
    Sem *s = *sem;
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->lock);
    free(s);
    *sem = NULL;
    return 0;
}
static ABI int32_t sem_wait_guest(void **sem) {
    if (!sem || !*sem) { set_errno(22); return -1; }
    Sem *s = *sem;
    pthread_mutex_lock(&s->lock);
    while (!s->value) pthread_cond_wait(&s->cond, &s->lock);
    --s->value;
    pthread_mutex_unlock(&s->lock);
    return 0;
}
static ABI int32_t sem_post_guest(void **sem) {
    if (!sem || !*sem) { set_errno(22); return -1; }
    Sem *s = *sem;
    pthread_mutex_lock(&s->lock);
    ++s->value;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

/* ---- Sockets: the network is unplugged ---- */
static ABI int64_t no_network(void) { set_errno(50 /* ENETDOWN */); return -1; }
/* inet_pton: dotted IPv4 only (AF_INET = 2); anything else is "not an address". */
static ABI int32_t inet_pton_guest(int af, const char *src, void *dst) {
    if (af != 2) return 0;
    if (!src || !dst) return 0;
    uint8_t out[4];
    for (int i = 0; i < 4; ++i) {
        if (*src < '0' || *src > '9') return 0;
        unsigned v = 0, digits = 0;
        while (*src >= '0' && *src <= '9') { v = v * 10 + (unsigned)(*src++ - '0'); if (++digits > 3 || v > 255) return 0; }
        out[i] = (uint8_t)v;
        if (i < 3 && *src++ != '.') return 0;
    }
    if (*src) return 0;
    memcpy(dst, out, 4);
    return 1;
}
static ABI int32_t chmod_guest(const char *path, uint32_t mode) { (void)path; (void)mode; return 0; }

/* ---- libkernel_unity: hardware exception handlers (PS4Util's crash reporter) ---- */
static void *exception_handlers[64];
static ABI int32_t install_exception_handler(int signal, void *handler) {
    if (signal < 0 || signal >= 64) return ERR(22);
    exception_handlers[signal] = handler;
    printf("Runtime: exception handler for signal %d noted (not hooked)\n", signal);
    return 0;
}
static ABI int32_t raise_exception(void *thread, int signal) {
    printf("Runtime: sceKernelRaiseException(%p, %d) not delivered\n", thread, signal);
    return 0;
}

/* ---- Services with no PC counterpart ---- */
static ABI int32_t not_supported(void) { return (int32_t)0x80020016; }            /* EINVAL */
static ABI int32_t pad_invalid(void) { return (int32_t)0x80920001; }              /* SCE_PAD_ERROR_INVALID_ARG */
static ABI int32_t move_no_device(void) { return (int32_t)0x80EE0003; }           /* SCE_MOVE_ERROR_INVALID_HANDLE */
static ABI int32_t vr_not_ready(void) { return (int32_t)0x81200001; }             /* SCE_VR_TRACKER_ERROR_NOT_INIT */
static ABI int32_t camera_invalid(void) { return (int32_t)0x802E0003; }           /* SCE_CAMERA_ERROR_PARAM */
static ABI int32_t dialog_unavailable(void) { return (int32_t)0x80B80001; }       /* COMMON_DIALOG not available */
static ABI int32_t ssl_unavailable(void) { return (int32_t)0x8095F002; }          /* SCE_SSL_ERROR_NOT_INITIALIZED */
static ABI int32_t np_signed_out(void) { return (int32_t)0x80550006; }            /* SCE_NP_ERROR_SIGNED_OUT */
static ABI int32_t matching_not_ready(void) { return (int32_t)0x80550C01; }       /* SCE_NP_MATCHING2 not initialized */
static ABI int32_t addcont_not_found(void) { return (int32_t)0x80D90005; }        /* SCE_APP_CONTENT_ERROR_NOT_FOUND */
static ABI int32_t addcont_not_mounted(void) { return (int32_t)0x80D90007; }      /* SCE_APP_CONTENT_ERROR_NOT_MOUNTED */
static ABI int32_t space_kb(const void *mount_point, uint64_t *kb) {
    (void)mount_point;
    if (!kb) return (int32_t)0x80D90002; /* SCE_APP_CONTENT_ERROR_PARAMETER */
    *kb = 1024 * 1024;                   /* 1 GB free */
    return 0;
}
static ABI const char *ajm_str_error(int32_t code) { (void)code; return "AJM error"; }

static const RuntimeExport exports[] = {
    {"sem_init", sem_init_guest}, {"sem_destroy", sem_destroy_guest}, {"sem_wait", sem_wait_guest},
    {"sem_post", sem_post_guest},
    {"sendmsg", no_network}, {"recvmsg", no_network}, {"inet_pton", inet_pton_guest}, {"chmod", chmod_guest},
    {"sceKernelInstallExceptionHandler", install_exception_handler}, {"sceKernelRaiseException", raise_exception},
    {"sceKernelSetGPO", ok}, {"sceKernelSetVirtualRangeName", ok},
    /* System service toggles and reports */
    {"sceSystemServiceDisablePersonalEyeToEyeDistanceSetting", ok},
    {"sceSystemServiceEnablePersonalEyeToEyeDistanceSetting", ok},
    {"sceSystemServiceReportAbnormalTermination", ok}, {"sceSystemServiceShowControllerSettings", ok},
    {"sceSystemServiceEnableSuspendConfirmationDialog", ok}, {"sceSystemServiceDisableSuspendConfirmationDialog", ok},
    {"sceSystemServicePowerTick", ok},
    /* Social screen (PS VR second screen) */
    {"sceSocialScreenInitializeSeparateModeParameter", ok}, {"sceSocialScreenSetMode", ok},
    {"sceSocialScreenOpenSeparateMode", not_supported}, {"sceSocialScreenCloseSeparateMode", ok},
    {"sceSocialScreenConfigureSeparateMode", not_supported},
    {"sceSocialScreenDialogInitialize", ok}, {"sceSocialScreenDialogTerminate", ok},
    {"sceSocialScreenDialogOpen", dialog_unavailable}, {"sceSocialScreenDialogClose", ok},
    {"sceSocialScreenDialogUpdateStatus", ok}, {"sceSocialScreenDialogGetResult", dialog_unavailable},
    /* PS VR */
    {"sceVrTrackerCpuProcess", vr_not_ready}, {"sceVrTrackerNotifyEndOfCpuProcess", vr_not_ready},
    {"sceVrTrackerGetPlayAreaWarningInfo", vr_not_ready}, {"sceVrTrackerGpuWait", vr_not_ready},
    {"sceVrTrackerResetOrientationRelative", vr_not_ready},
    {"sceHmdReprojectionSetUserEventEnd", ok}, {"sceHmdReprojectionStartMultilayer", ok},
    {"sceHmdReprojectionClearUserEventStart", ok},
    /* PS Move: the library starts, no controller opens */
    {"sceMoveInit", ok}, {"sceMoveTerm", ok}, {"sceMoveOpen", move_no_device},
    {"sceMoveReadStateRecent", move_no_device}, {"sceMoveSetLightSphere", move_no_device},
    {"scePadDeviceClassParseData", pad_invalid}, {"scePadDeviceClassGetExtendedInformation", pad_invalid},
    {"sceCameraIsValidFrameData", camera_invalid},
    {"sceVideoOutConfigureOptionsInitialize_", ok}, {"sceAudioOutSetMixLevelPadSpk", ok},
    {"sceAudio3dAudioOutClose", ok}, {"sceAjmStrError", ajm_str_error},
    /* Remote Play and live streaming */
    {"sceRemoteplayApprove", ok}, {"sceRemoteplayProhibit", ok}, {"sceRemoteplayProhibitStreaming", ok},
    {"sceGameLiveStreamingPermitLiveStreaming", ok},
    /* Add-on content: none */
    {"sceAppContentAddcontMount", addcont_not_found}, {"sceAppContentAddcontUnmount", addcont_not_mounted},
    {"sceAppContentTemporaryDataUnmount", ok},
    {"sceAppContentTemporaryDataGetAvailableSpaceKb", space_kb}, {"sceAppContentDownloadDataGetAvailableSpaceKb", space_kb},
    /* Network services: offline */
    {"sceHttpSetRedirectCallback", ok}, {"sceHttpSetAuthEnabled", ok}, {"sceNpWebApiAddHttpRequestHeader", ok},
    {"sceSslGetCaCerts", ssl_unavailable}, {"sceSslGetMemoryPoolStats", ssl_unavailable},
    {"sceSslGetIssuerName", ssl_unavailable}, {"sceSslGetSerialNumber", ssl_unavailable},
    {"sceSslGetSubjectName", ssl_unavailable}, {"sceSslGetNameEntryCount", ssl_unavailable},
    {"sceSslGetNameEntryInfo", ssl_unavailable}, {"sceSslFreeSslCertName", ok},
    {"sceNpMatching2GetRoomPasswordLocal", matching_not_ready},
    {"sceNpTusGetFriendsDataStatusA", np_signed_out}, {"sceNpTusGetFriendsDataStatusForCrossSave", np_signed_out},
    {"sceNpTusGetFriendsVariableA", np_signed_out}, {"sceNpTusGetFriendsVariableForCrossSave", np_signed_out},
    {"sceNpTusGetMultiUserDataStatusA", np_signed_out}, {"sceNpTusGetMultiUserDataStatusAVUser", np_signed_out},
    {"sceNpTusGetMultiUserDataStatusForCrossSave", np_signed_out},
    {"sceNpTusGetMultiUserDataStatusForCrossSaveVUser", np_signed_out},
    {"sceNpTusGetMultiUserVariableA", np_signed_out}, {"sceNpTusGetMultiUserVariableAVUser", np_signed_out},
    {"sceNpTusGetMultiUserVariableForCrossSave", np_signed_out},
    {"sceNpTusGetMultiUserVariableForCrossSaveVUser", np_signed_out},
    {"sceNpTusTryAndSetVariableA", np_signed_out}, {"sceNpTusTryAndSetVariableAVUser", np_signed_out},
    {"sceNpTusTryAndSetVariableForCrossSave", np_signed_out},
    {"sceNpTusTryAndSetVariableForCrossSaveVUser", np_signed_out},
};
uintptr_t runtime_unity_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }

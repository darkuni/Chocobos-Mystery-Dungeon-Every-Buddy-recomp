/* Module records for libc's C++ exception unwinder (libunwind) and for
 * sceKernelGetModuleInfoFromAddr. IL2CPP turns every managed exception into a C++ throw; to find
 * the catch, the unwinder asks the kernel which module holds each return address and where its
 * .eh_frame_hdr and .eh_frame are (sceKernelGetModuleInfoForUnwind). Without an answer the first
 * throw ends in std::terminate (sceKernelDebugRaiseException 0xa0020008).
 * The probe registers the eboot and every linked module (dynlib.c); .eh_frame is found through
 * the header's eh_frame_ptr, its size as shadPS4 computes it (module.cpp). */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

#define ERR(n) ((int32_t)(UINT32_C(0x80020000) | (n)))
#define MAX_MODULES 32

typedef struct {
    char name[256];
    uintptr_t base, end;
    uintptr_t eh_hdr, eh_frame;
    uint64_t eh_hdr_size, eh_frame_size;
    int segments;
    uint64_t seg_addr[4], seg_size[4];
    uint32_t seg_prot[4];
    int32_t id;
} ModuleRecord;
static ModuleRecord modules[MAX_MODULES];
static int module_count;
static uint64_t unwind_queries, unwind_misses;

/* A DW_EH_PE-encoded pointer at p (pcrel/datarel/absolute; 2-, 4- and 8-byte forms). */
static int read_encoded(const uint8_t *p, uint8_t encoding, uintptr_t datarel, uintptr_t *out) {
    if (encoding == 0xff) return -1;
    int64_t v;
    switch (encoding & 0x0f) {
    case 0x00: case 0x04: { uint64_t x; memcpy(&x, p, 8); v = (int64_t)x; break; }
    case 0x0c: { int64_t x; memcpy(&x, p, 8); v = x; break; }
    case 0x03: { uint32_t x; memcpy(&x, p, 4); v = x; break; }
    case 0x0b: { int32_t x; memcpy(&x, p, 4); v = x; break; }
    case 0x02: { uint16_t x; memcpy(&x, p, 2); v = x; break; }
    case 0x0a: { int16_t x; memcpy(&x, p, 2); v = x; break; }
    default: return -1;
    }
    switch (encoding & 0x70) {
    case 0x00: break;
    case 0x10: v += (int64_t)(uintptr_t)p; break;
    case 0x30: v += (int64_t)datarel; break;
    default: return -1;
    }
    *out = (uintptr_t)v;
    return 0;
}

void runtime_module_register(const char *name, uintptr_t base, uint64_t span, uint64_t eh_hdr_vaddr,
                             uint64_t eh_hdr_size, int segments, const uint64_t *seg_vaddr,
                             const uint64_t *seg_size, const uint32_t *seg_prot) {
    if (module_count == MAX_MODULES) return;
    ModuleRecord *m = &modules[module_count];
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->base = base;
    m->end = base + ((span + 0x3fff) & ~(uint64_t)0x3fff);
    m->id = module_count + 1;
    m->segments = segments > 4 ? 4 : segments;
    for (int i = 0; i < m->segments; ++i) {
        m->seg_addr[i] = base + seg_vaddr[i];
        m->seg_size[i] = seg_size[i];
        m->seg_prot[i] = seg_prot[i];
    }
    if (eh_hdr_size >= 8) {
        const uint8_t *hdr = (const uint8_t *)(base + eh_hdr_vaddr);
        uintptr_t frame;
        if (hdr[0] == 1 && !read_encoded(hdr + 4, hdr[1], (uintptr_t)hdr, &frame)) {
            m->eh_hdr = (uintptr_t)hdr;
            m->eh_hdr_size = eh_hdr_size;
            m->eh_frame = frame;
            m->eh_frame_size = frame < m->eh_hdr ? m->eh_hdr - frame : m->end - m->eh_hdr;
        }
    }
    ++module_count;
    printf("Runtime: module %s at %p-%p, .eh_frame_hdr %p, .eh_frame %p (%llu bytes)\n", m->name,
           (void *)m->base, (void *)m->end, (void *)m->eh_hdr, (void *)m->eh_frame,
           (unsigned long long)m->eh_frame_size);
}

static const ModuleRecord *find(uintptr_t address) {
    for (int i = 0; i < module_count; ++i)
        if (address >= modules[i].base && address < modules[i].end) return &modules[i];
    return NULL;
}

typedef struct {
    uint64_t size;
    char name[256];
    uint64_t eh_frame_hdr, eh_frame, eh_frame_size, seg0_addr, seg0_size;
} UnwindInfo;
_Static_assert(sizeof(UnwindInfo) == 0x130, "OrbisModuleInfoForUnwind");
static ABI int32_t module_info_for_unwind(uintptr_t address, int32_t flags, UnwindInfo *info) {
    if (flags >= 3) { if (info) memset(info, 0, sizeof(*info)); return ERR(22); }
    if (!info) return ERR(14);
    if (info->size < sizeof(*info)) return ERR(22);
    __atomic_add_fetch(&unwind_queries, 1, __ATOMIC_RELAXED);
    const ModuleRecord *m = find(address);
    if (!m || !m->eh_hdr) {
        uint64_t n = __atomic_add_fetch(&unwind_misses, 1, __ATOMIC_RELAXED);
        if (n <= 8) printf("Runtime: unwind lookup of %p: no module\n", (void *)address);
        return ERR(3);
    }
    uint64_t size = info->size;
    memset(info, 0, sizeof(*info));
    info->size = size;
    snprintf(info->name, sizeof(info->name), "%s", m->name);
    info->eh_frame_hdr = m->eh_hdr;
    info->eh_frame = m->eh_frame;
    info->eh_frame_size = m->eh_frame_size;
    info->seg0_addr = m->seg_addr[0];
    info->seg0_size = m->seg_size[0];
    return 0;
}

typedef struct { uint64_t address; uint32_t size; int32_t prot; } SegmentInfo;
typedef struct {
    uint64_t size;
    char name[256];
    int32_t id;
    uint32_t tls_index;
    uint64_t tls_init_addr;
    uint32_t tls_init_size, tls_size, tls_offset, tls_align;
    uint64_t init_proc, fini_proc, reserved1, reserved2;
    uint64_t eh_frame_hdr, eh_frame;
    uint32_t eh_frame_hdr_size, eh_frame_size;
    SegmentInfo segments[4];
    uint32_t segment_count;
} ModuleInfoEx;
static ABI int32_t module_info_from_addr(uintptr_t address, int32_t flags, ModuleInfoEx *info) {
    if (flags >= 3) { if (info) memset(info, 0, sizeof(*info)); return ERR(22); }
    if (!info) return ERR(14);
    const ModuleRecord *m = find(address);
    if (!m) return ERR(3);
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    snprintf(info->name, sizeof(info->name), "%s", m->name);
    info->id = m->id;
    info->eh_frame_hdr = m->eh_hdr;
    info->eh_frame = m->eh_frame;
    info->eh_frame_hdr_size = (uint32_t)m->eh_hdr_size;
    info->eh_frame_size = (uint32_t)m->eh_frame_size;
    for (int i = 0; i < m->segments; ++i) {
        info->segments[i].address = m->seg_addr[i];
        info->segments[i].size = (uint32_t)m->seg_size[i];
        info->segments[i].prot = (int32_t)m->seg_prot[i];
    }
    info->segment_count = (uint32_t)m->segments;
    return 0;
}

static const RuntimeExport exports[] = {
    {"sceKernelGetModuleInfoForUnwind", module_info_for_unwind},
    {"sceKernelGetModuleInfoFromAddr", module_info_from_addr},
};
uintptr_t runtime_modules_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }

/* First-start preparation of a game folder for the loader: the C version of scripts/prepare.py +
 * link_modules.py + function_starts.py + content_profile.py. Writes, into out_dir:
 *   boot-linked.bin (G3PROBE5: eboot + the game's own modules, linked), functions.bin (BBFUNC01:
 *   function starts for red-zone protection on Windows), content.bin (G3CONT01, SKU full).
 * Byte-for-byte the same files as the Python scripts (tests/test_boot_prepare.sh checks). */
#pragma once
#include <stddef.h>

/* modules: the game's sce_module files to link, in order (NULL-terminated). 0 on success. */
int boot_prepare(const char *game_dir, const char *out_dir, const char *const *modules, char *err, size_t err_size);

/* Exported functions and data of a PRX: fn(ctx, NID, module-relative address) per export. */
#include <stdint.h>
int boot_module_exports(const char *path, void (*fn)(void *ctx, const char *nid, uint64_t value), void *ctx);

/* Loaded segments (PT_LOAD and PT_SCE_RELRO, first four) and .eh_frame_hdr of a PRX or eboot. */
typedef struct {
    uint64_t span, eh_hdr_vaddr, eh_hdr_size;
    int segments;
    uint64_t seg_vaddr[4], seg_size[4];
    uint32_t seg_prot[4];
} BootModuleLayout;
int boot_module_layout(const char *path, BootModuleLayout *out);
